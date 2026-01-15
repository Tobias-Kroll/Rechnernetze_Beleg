#define _POSIX_C_SOURCE 200112L // Testweise eingebaut: könnte Fehler vermeiden
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/select.h>

#include "data.h"
#include "config.h"
#include "clientSy.h"

/* --------------------------------------------------------------- */
/*  Globale Transport-Variablen                                    */
/* --------------------------------------------------------------- */

static int g_sock = -1; // UDP-Socket des Clients
static struct sockaddr_storage g_srv; // Server-Zieladresse (IPv6)
static socklen_t g_srvlen = 0; // Länge der Socket-/Zieladresse

static unsigned long g_base = 0; // Fensterbasis (ältestes unbestätigtes Paket)
static unsigned long g_next = 0; // nächste Sequenznummer (neu zu senden)
static int g_win = 1; // aktuelle Fenstergröße 
static int g_inFlight = 0; // Anzahl unbestätigter Pakete im Fenster

static struct request g_wbuf[GBN_BUFFER_SIZE]; // Ringpuffer für gesendete Requests
static int g_wvalid[GBN_BUFFER_SIZE]; // Slot belegt? (0/1)

/* --------------------------------------------------------------- */
/*  Go-Back-N Sender State                                         */
/* --------------------------------------------------------------- */

static int g_timer_units = 0; // Timer für ältestes unbestätigtes Paket (in Slots)
static int g_retx_active = 0; // 1 = gerade im Retransmit-Modus (Timeout passiert)
static unsigned long g_retx_next = 0; // nächste Sequenznummer, die retransmittet wird

// Ringpuffer-Index aus Sequenznummer berechnen
static inline int idxOf(unsigned long seq) {
    return (int)(seq % GBN_BUFFER_SIZE);
}



static void resetSenderState(int winSize) {
    // Fenstergröße in erlaubten Bereich bringen
    if (winSize < 1) winSize = 1;
    if (winSize > GBN_MAX_WINDOW) winSize = GBN_MAX_WINDOW;

    // Go-Back-N Fensterzustand
    g_win = winSize;
    g_base = 0;
    g_next = 0;
    g_inFlight = 0;

    // Timer / Retransmit-Zustand 
    g_timer_units = 0;
    g_retx_active = 0;
    g_retx_next = 0;

    // Ringpuffer-Slots als "leer" makieren
    memset(g_wvalid, 0, sizeof(g_wvalid));
}



static int seqInWindow(unsigned long ackSeNo) {
    // ackSeNo ist "next expected:" gültig wenn base < ackSeNo <= base + inFlight

    if (g_inFlight <= 0) return 0; // nichts ausstehend -> ACK uninteressant

    if (ackSeNo <= g_base) return 0; // zu alt / Duplicate ACK

    if (ackSeNo > (g_base + (unsigned long)g_inFlight)) return 0; // zu neu / außerhalb

    return 1; // ACK ist im Fenster -> akzeptiert
}



static void slideWindowTo(unsigned long newBase) {
    // newBase ist ackSeNo ("next expected")

    while (g_base < newBase) {
        int i = idxOf(g_base); // Ringpuffer-Slot für das Paket g_base
        g_wvalid[i] = 0; // Slot freigeben: Paket gilt als bestätigt 

        g_base++; // Fensterbasis nach vorne schieben
        g_inFlight--; // eins weniger "in flight"
    }

    // Timer / Retransmit-Status anpassen
    if (g_inFlight > 0) {
        // es gibt noch unbestätigte Pakete -> Timer neu starten für das neue "älteste"
        g_timer_units = GBN_TIMEOUT_UNITS;
    } else {
        // Fenster ist leer -> kein Timer nötig, keine Retransmits aktiv
        g_timer_units = 0;
        g_retx_active = 0;
        g_retx_next = 0;
    }
}



static int sendPacket(const struct request *req) {
    // Sende genau ein Request-Paket an den Server
    ssize_t sent = sendto(g_sock, req, sizeof(*req), 0, (struct sockaddr *)&g_srv, g_srvlen);
    // sendto() fehlgeschlagen
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    // Sicherheitscheck: UDP sollte das komplette Paket senden
    if ((size_t)sent != sizeof(*req)) {
        fprintf(stderr, "sendto:only %zd of %zu bytes sent\n",sent, sizeof(*req));
        return -1;
    }
    return 0;
}



// Warten bis ACK oder Slotende. Bei frühem ACK: idle bis Slotende.
static int waitForAckOneSlot(struct answer *outAns) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_sock, &rfds);

    struct timeval tv;
    long total_us = (long)GBN_TIMEOUT_INT_MS * 1000L;
    tv.tv_sec = total_us / 1000000L;
    tv.tv_usec = total_us % 1000000L;

    int rc = select(g_sock + 1, &rfds, NULL, NULL, &tv);
    if (rc < 0) {
        if (errno == EINTR) return 0; // Signal -> Slot wie "kein ACK" behandeln
        perror("select");
        return -1;
    }
    if (rc == 0) {
        return 0; // Slot vorbei, kein ACK
    }

    // ACK ist da
    memset(outAns, 0, sizeof(*outAns));
    ssize_t got = recvfrom(g_sock, outAns, sizeof(*outAns), 0, NULL, NULL);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        perror("recvfrom");
        return -1;
    }
    if ((size_t)got != sizeof(*outAns)) {
        fprintf(stderr, "recvfrom: wrong answer size %zd (expected %zu)\n", got, sizeof(*outAns));
        return 0; // falsche Größe -> ignoriert
    }

    // bis Slotende idle (Restzeit tv enthält Resr nach select)
    if(tv.tv_sec > 0 || tv.tv_usec > 0) {
        (void)select(0, NULL, NULL, NULL, &tv);
    }

    return 1; // ACK gelesen
}

/*
 * clientSy.c
 *
 * Diese Datei soll die ARQ-/Go-Back-N-Sende- und das UDP Handling
 * für den Client enthalten.
 *
 * WICHTIG:
 *   - Dateiname und Funktionssignaturen in clientSy.h sollen
 *     unverändert beibehalten werden.
 *
 * Aufgaben:
 *   - UDP-Socket (IPv6, Datagram) erzeugen, Serveradresse auflösen
 *   - ARQ-/GBN-Sendealgorithmus implementieren:
 *        * Fensterverwaltung (base, nextNum, packetCount)
 *        * innerhal eines Intervalls max. 1 Paket senden und empfangen (GBN_TIMEOUT_INT_MS)
 *        * Paket-Timeout in Intervallen (GBN_TIMEOUT_UNITS)
 *        * Retransmission der unbestätigten Pakete
 *   - kumulative ACKs (AnswOk.SeNo) auswerten
 *   - Hello/Data/Close über die gemeinsame Logik abwickeln
 */

/* Optionale globale Variablen:
 *   - Socket-Deskriptor
 *   - Serveradresse
 *   - GBN-Sendezustand (Fensterbasis, nächster freier Platz etc.)
 */

/* --------------------------------------------------------------- */
/*  Initialisierung / Abschluss                                   */
/* --------------------------------------------------------------- */


void initClient(char *name, const char *port)
{
    const char *server = (name != NULL) ? name : DEFAULT_LOOPBACK_HOST; //Host wählen
    struct addrinfo hints; //Filter für getaddrinfo
    struct addrinfo *res = NULL; //Ergebnisliste

    memset(&hints, 0, sizeof(hints)); //alles auf 0 setzen
    hints.ai_family = AF_INET6; // IPv6
    hints.ai_socktype = SOCK_DGRAM; //UDP Datagram
    hints.ai_protocol = IPPROTO_UDP; //UDP Protokoll
    
    int rc = getaddrinfo(server, port, &hints, &res); //Host+Port auflösen
    if (rc != 0 || res == NULL) { //rc != 0 bedeutet Fehler
        fprintf(stderr, "getaddrinfo(%s,%s) failed: %s\n", server, port, gai_strerror(rc)); //Fehlertext holen
        exit(EXIT_FAILURE); //Programm sauber beenden
    }

    g_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol); //UDP/IPv6 Socket
    if (g_sock < 0) { // socket() liefert -1 bei Fehler
        perror("socket"); //nutzt errno -> druckt System-Fehlertext
        freeaddrinfo(res); //Speicher von getadrrinfo freigeben
        exit(EXIT_FAILURE); //abbrechen, ohne Socket geht nichts
    }

    memcpy(&g_srv, res->ai_addr, res->ai_addrlen); //Zieladresse speichern
    g_srvlen = (socklen_t)res->ai_addrlen;

    freeaddrinfo(res); //getaddrinfo-Liste freigeben
    res = NULL; //Pointer "sicher" machen ("dangling pointer" verhindern)

    int flags = fcntl(g_sock, F_GETFL, 0); //aktuelle Flags holen
    if (flags < 0){ //Fehler (bei fcntl = -1)
        perror("fcntl(F_GETFL)"); //Systemfehler ausgeben
        close(g_sock); //Socket schließen (sonst leak)
        g_sock = -1; //als ungültig makieren
        exit(EXIT_FAILURE); //abbrechen
    }

    if (fcntl(g_sock, F_SETFL, flags | O_NONBLOCK) < 0) { //non-blocking setzen (mit bitweise ODER: "|")
        perror("fcntl(F_SETFL)"); //Systemfehler ausgeben
        close(g_sock); //Socket schließen
        g_sock = -1; //ungültig makieren
        exit(EXIT_FAILURE);//abbrechen
    }
}



void closeClient(void)
{
    //Platzhalter:
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_srvlen = 0;
    memset(&g_srv, 0, sizeof(g_srv));
    resetSenderState(1);
}
    

/* --------------------------------------------------------------- */
/*  Interne Sende-Logik (GBN/ARQ)                                  */
/* --------------------------------------------------------------- */

/*
 * doRequest:
 *   - ReqHello: einmaliger Handshake mit eigenem Timeout
 *   - ReqData / ReqClose: Go-Back-N-Sendealgorithmus mit Fenster 
 *
 * Parameter:
 *   req        : zu sendendes Request-Paket
 *   winSize    : Fenstergröße (1..GBN_MAX_WINDOW)
 *   windowFull : optionaler Rückgabewert, ob das Sendefenster voll ist
 *
 * Rückgabewert:
 *   - Zeiger auf empfangene Antwort (struct answer)
 *   - NULL, wenn in diesem Intervall keine relevante Antwort
 *     eingetroffen ist
 */



static struct answer *doRequest(struct request *req, int winSize, int *windowFull, int *retransmission) {

    static struct answer ans;

    // Rückgabeflags defaulten
    if (windowFull) *windowFull = 0;
    if (retransmission) *retransmission = 0;

    // Fenstergröße nur clampen (Reset passiert z.B. in arqSendHello via resetSenderState)
    if (winSize < 1) winSize = 1;
    if (winSize > GBN_MAX_WINDOW) winSize = GBN_MAX_WINDOW;
    g_win = winSize;

    /* ------------------ (1) Sendephase: max 1 Paket pro Slot ------------------ */
    if (g_retx_active) {
        // Timeout-Modus: Go-Back-N Retransmit ab Basis,pro Slot genau 1 Paket
        if (g_retx_next < g_next) {
            int bi = idxOf(g_retx_next);
            if (g_wvalid[bi]) {
                if (sendPacket(&g_wbuf[bi]) < 0) return NULL;
                if (retransmission) *retransmission = 1;
            }
            g_retx_next++; // im nächsten Slot nächstes paket retransmitten
        }else {
            // Alle unbestätigten Pakete einmal retransmittieren -> zurück in Normalmodus
            g_retx_active = 0;
        }
    }else {
        // Normalmodus: wenn Platz im Fenster, pro Slot höchstens 1 neues Paket senden
        if (req != NULL) {
            if (g_inFlight >= g_win) {
                if (windowFull) *windowFull = 1; // Senderfenster voll
            }else {
                // Erwartung: Aufrufer liefert forlaufende SeNr passend zu g_next
                if (req->SeNr != g_next) {
                    fprintf(stderr, "doRequest: unexpected SeNr=%lu, expected %lu\n", req->SeNr, g_next);
                }else {
                    int ni = idxOf(g_next);

                    // Paket im Ringpuffer speichern, damit es bei Timeout retransmittiert werden kann
                    g_wbuf[ni] = *req;
                    g_wvalid[ni] = 1;

                    // senden
                    if (sendPacket(&g_wbuf[ni]) < 0) return NULL;

                    // Fensterzustand aktualisieren
                    g_next++;
                    g_inFlight++;

                    // Timer läuft immer nur für das älteste unbestätigte Paket
                    if (g_inFlight == 1) {
                        g_timer_units = GBN_TIMEOUT_UNITS;
                    }
                }
            }
        }   
    }

    /* ------------------ (2) Empfangsphase: ACK oder Slotende ------------------ */
    int wrc = waitForAckOneSlot(&ans); // select() wartet bis ACK oder Slotende; bei frühem ACK idle
    if (wrc < 0) return NULL;

    int  haveAns = (wrc == 1);

    if (haveAns) {
        // Kumulative ACKs auswerten (SeNo = next expected)
        if (ans.AnswType == AnswOk || ans.AnswType == AnswHello) {
            unsigned long ack = ans.SeNo;

            // ACK nur aktzeptieren, wenn es im aktuellen Fenster liegt
            if (seqInWindow(ack)) {
                slideWindowTo(ack); // bestätigt alles < ack

                // Wenn retransmittiert wird und Fenster vorgeschoben wurde, darf g_retx nicht hinter (also <) der neuen Basis liegen
                if (g_retx_active && g_retx_next < g_base) {
                    g_retx_next = g_base;
                }
            }else {
                //außerhalb Fenster -> ignorieren
            }
        }
        // Warn/Err: verändert das Fenster nicht
    }
     
     /* ------------------ (3) Slotende: Timer dekrementieren / Timeout ------------------ */
     if (g_inFlight > 0) {
        // In JEDEM Fall ist 1 Intervall vergangen (auch wenn ACK früh kam -> idle bis Slotende)
        g_timer_units--; // ein Zeitslot vergangen

        if (g_timer_units <= 0) {
            // Timeout -> Go-Back-N: Retransmit ab base, 1 Paket pro Slot
            g_retx_active = 1;
            g_retx_next = g_base;
            g_timer_units = GBN_TIMEOUT_UNITS;

            if (retransmission) *retransmission = 1;
        }
     }

     return haveAns ? &ans : NULL; 
}


/* --------------------------------------------------------------- */
/*  Externe ARQ-API (Wrapper um doRequest)                         */
/* --------------------------------------------------------------- */


int arqSendHello(int winSize)
{
    struct request req; //Request-Paket anlegen (lokal auf dem Stack)
    memset(&req, 0, sizeof(req)); //alles auf 0, damit keine Zufallswerte drin sind

    // Senderzustand komplett resetten (Fenster, Timer, Retransmit, Ringpuffer)
    resetSenderState(winSize);

    // Hello-Request vorbereiten
    req.ReqType = ReqHello; //HELLO-Pakettyp setzen
    req.FlNr = 0; //bei HELLO keine Nutzdatenlänge

    // Wichtig: SeNr muss zum Senderzustand passen (erstes Paket: g_next == 0)
    req.SeNr = g_next; //bei HELLO keine Sequenznummer nötig

    int windowFull = 0;
    int retransmission = 0;

    int hello_sent = 0;

    // Slot-Schleife: pro Intervall max 1 neues Paket; wenn ACK früh kommt -> idle passiert in waitForAckOneSlot()
    for (;;) {
        struct answer *ans;
        
        if (!hello_sent) {
            // Erstes Intervall: Hello als "neues Paket" in den ARQ-Core geben
            ans = doRequest(&req, g_win, &windowFull, &retransmission);
            hello_sent = 1;
        }else {
            // Danach: keine neuen Pakete, nur ARQ weiterlaufen lassen (ACKs/Timeout/Retx)
            ans = doRequest(NULL, g_win, &windowFull, &retransmission);
        }

        // doRequest liefert NULL, wenn in diesem Slot kein ACK kam -> weiter im nächsten Slot
        if (ans == NULL) {
            continue;
        }

        // Antwort auswerten
        if (ans->AnswType == AnswHello || ans->AnswType == AnswOk) {
            return 0; // Erfolg
        }
        if (ans->AnswType == AnswErr) {
            return 1; // Serverfehler -> abbrechen
        }

        // Warnung o.ä. ignorieren hier und weiter warten
    }
}



int arqSendData(const struct app_unit *app, int winSize) {

    if (app == NULL) return 1;

    // Fenstergröße clampen
    if (winSize < 1) winSize = 1;
    if (winSize > GBN_MAX_WINDOW) winSize = GBN_MAX_WINDOW;

    // Request aus app_unit bauen
    struct request req;
    memset(&req, 0, sizeof(req));

    req.ReqType = ReqData;

    // Länge begrenzen (BufferSize aus data.h)
    unsigned long len = app->len;
    if (len > (unsigned long)BufferSize) len = (unsigned long)BufferSize;
    req.FlNr = len;

    // Sequenznummer für dieses (genau ein) Datenpaket festlegen
    // Wichtig: beim ersten neuen Senden muss req.SeNr == g_next sein
    unsigned long mySeq = g_next;
    req.SeNr = mySeq;

    // Payload kopieren (req.name als Datenfeld)
    if (len > 0) {
        memcpy(req.name, app->data, (size_t)len);
    }
    // Rest ist bereits 0 durch memset

    int windowFull = 0;
    int retransmission = 0;

    int queued = 0; // wurde dieses Paket bereits wirklich als neues Paket gesendet/eingequeued?

        for (;;) {
        // Erfolg: Unser Paket ist kumulativ bestätigt -> base ist über unsere SeNr hinaus
        if (g_base > mySeq) {
            return 0;
        }

        struct answer *ans;

        if (!queued) {
            // Paket als "neu" anbieten: doRequest sendet es nur, wenn Fenster Platz hat
            ans = doRequest(&req, winSize, &windowFull, &retransmission);

            // Wenn Fenster nicht voll war und doRequest es als neues Paket gesendet hat,
            // dann wurde g_next hochgezählt -> req.SeNr < g_next bedeutet: "eingereiht"
            if (!windowFull && req.SeNr < g_next) {
                queued = 1;
            }
        } else {
            // Unser Paket ist schon unterwegs -> keine neuen Pakete mehr,
            // nur ACKs/Timeout/Go-Back-N-ReTx weiter abarbeiten
            ans = doRequest(NULL, winSize, &windowFull, &retransmission);
        }

        // Antwort auswerten (falls in diesem Slot etwas kam)
        if (ans != NULL) {
            if (ans->AnswType == AnswErr) {
                unsigned long code = ans->ErrNo;
                if (code < 8) {
                    fprintf(stderr, "arqSendData: server error %lu (%s)\n", code, errorTable[code]);
                } else {
                    fprintf(stderr, "arqSendData: server error %lu\n", code);
                }
                return 1;
            }

            if (ans->AnswType == AnswWarn) {
                unsigned long code = ans->ErrNo;
                if (code < 8) {
                    fprintf(stderr, "arqSendData: warning %lu (%s)\n", code, errorTable[code]);
                } else {
                    fprintf(stderr, "arqSendData: warning %lu\n", code);
                }
                // Warn ist nicht zwingend fatal -> weiterlaufen bis bestätigt
            }

            // AnswOk/AnswHello:
            // Fenster-Sliding passiert bereits in doRequest() über slideWindowTo()
        }

        // ans == NULL -> kein ACK in diesem Slot, nächster Slot
    }
}



int arqSendClose(int winSize)
{
    // Fenstergröße clampen (ARQ-State bleibt erhalten)
    if (winSize < 1) winSize = 1;
    if (winSize > GBN_MAX_WINDOW) winSize = GBN_MAX_WINDOW;

    struct request req;
    memset(&req, 0, sizeof(req));

    req.ReqType = ReqClose;
    req.FlNr = 0;

    // Close ist ein normales GBN-Paket mit eigener Sequenznummer
    // doRequest erwartet: req.SeNr == g_next, wenn es als neues Paket gesendet wird.
    unsigned long mySeq = g_next;
    req.SeNr = mySeq;

    //Nutzdaten bei Close nicht relevant, aber sauber nullen
    memset(req.name, 0, sizeof(req.name));

    int windowFull = 0;
    int retransmission = 0;

    int queued = 0; // wurde Close bereits als neues Paket tatsächlich gesendet/eingequeued?

    for (;;) {
        // Erfolg: Close wurde kumulativ bestätigt, Fensterbasis ist weiter als unsere Close-Sequenz
        if (g_base > mySeq) {
            return 0;
        }

        struct answer *ans;

        if (!queued) {
            // Close als neues Paket anbieten (max 1 Sendung pro Slot)
            ans = doRequest(&req, winSize, &windowFull, &retransmission);

            // Wenn Fenster voll war, wurde es NICHT gesendet -> im nächsten Slot erneut versuchen
            // Wenn es gesendet wurde, erhöht doRequest g_next um 1
            if (!windowFull && req.SeNr < g_next) {
                queued = 1;
            }
        }else {
            // Kein neues Paket mehr: nur noch ACKs/Timeouts/Retx abarbeiten
            ans = doRequest(NULL, winSize, &windowFull, &retransmission);
        }

        // Antwort auswerten (falls in diesem Slot was kam
        if (ans != NULL) {
            if (ans->AnswType == AnswErr) {
                unsigned long code = ans->ErrNo;
                if (code < 8) {
                    fprintf(stderr, "arqSendClose: server error %lu (%s)\n", code, errorTable[code]);
                }else {
                    fprintf(stderr, "arqSendClose: server error %lu\n", code);
                }
                return 1;
            }

            if (ans->AnswType == AnswWarn) {
                unsigned long code = ans->ErrNo;
                if (code < 8) {
                    fprintf(stderr, "arqSendClose: warning %lu (%s)\n", code, errorTable[code]);
                }else {
                    fprintf(stderr, "arqSendClose: warning %lu\n", code);
                }
                // Warn ist nicht zwingend fatal -> weiterlaufen lassen bis Close bestätigt ist
            }
            // AnswOk/AnswHello: Fenster-Sliding passiert in doRequest() bereits
        }
        // wenn ans == NULL: kein ACK in diesem Slot -> nächster Slot
    }
}
