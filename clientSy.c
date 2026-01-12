#define _POSIX_C_SOURCE 200112L //Testweise eingebaut: könnte Fehler vermeiden
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

static unsigned long g_base = 0; //Fensterbasis (ältestes unbestätigtes Paket)
static unsigned long g_next = 0; //nächste Sequenznummer (neu zu senden)
static int g_win = 1; //aktuelle Fenstergröße 
static int g_inFlight = 0; //Anzahl unbestätigter Pakete im Fenster

static struct request g_wbuf[GBN_BUFFER_SIZE]; //Ringpuffer für gesendete Requests
static int g_wvalid[GBN_BUFFER_SIZE]; //Slot belegt? (0/1)

/* --------------------------------------------------------------- */
/*  Go-Back-N Sender State                                         */
/* --------------------------------------------------------------- */
static int g_timer_units = 0; //Timer für ältestes unbestätigtes Paket (in Slots)
static int g_retx_active = 0; //1 = gerade im Retransmit-Modus (Timeout passiert)
static unsigned long g_retx_next = 0; //nächste Sequenznummer, die retransmittet wird

//Ringpuffer-Index aus Sequenznummer berechnen
static inline int idxOf(unsigned long seq) {
    return (int)(seq % GBN_BUFFER_SIZE);
}

static void resetSenderState(int winSize) {
    //Fenstergröße in erlaubten Bereich bringen
    if (winSize < 1) winSize = 1;
    if (winSize > GBN_MAX_WINDOW) winSize = GBN_MAX_WINDOW;

    //Go-Back-N Fensterzustand
    g_win = winSize;
    g_base = 0;
    g_next = 0;
    g_inFlight = 0;

    //Timer / Retransmit-Zustand 
    g_timer_units = 0;
    g_retx_active = 0;
    g_retex_next = 0;

    //Ringpuffer-Slots als "leer" makieren
    memset(g_wvalid, 0, sizeof(g_wvalid));
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

    /* TODO:
     *  - mit getaddrinfo(...) die Serveradresse ermitteln
     *  - UDP/IPv6-Socket erzeugen
     *  - Socket non-blocking setzen (fcntl)
     *  - Adresse und Socket in globalen Variablen speichern
     */
}

void closeClient(void)
{
    //Platzhalter:
    {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_srvlen = 0;
    memset(&g_srv, 0, sizeof(g_srv));

    resetSenderState(1);
}
    
    /* TODO:
     *  - ggf. offenen Socket schließen
     *  - globale Zustandsvariablen zurücksetzen
     */
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
 *
 * TODO:
 *   - Für ReqHello:
 *        * Fensterzustand zurücksetzen
 *        * Hello-Paket einmal senden
 *        * mit select() und separatem Timeout auf Antwort warten
 *   - Für Data/Close:
 *        * Sende­fenster im Ringpuffer verwalten (base, nextNum, packetCount)
 *        * pro Intervall höchstens EIN Paket senden (neu oder Retransmit)
 *        * Timeout in Einheiten von Intervallen zählen
 *        * kumulative ACKs auswerten und Fensterbasis verschieben
 */
static struct answer *doRequest(struct request *req, int winSize, int *windowFull, int *retransmission)
{

    (void)winSize;
    if (windowFull) *windowFull = 0;
    if (retransmission) *retransmission = 0;
  
    // 1) Request einmal senden
    ssize_t sent = sendto(g_sock, req, sizeof(*req), 0, (struct sockaddr *)&g_srv, g_srvlen); //Request senden
    if (sent < 0) { //Fehler
        perror("sendto"); //Systemfehler (errno)
        return NULL; //doRequest signalisiert Fehler
    }

    if ((size_t)sent != sizeof(*req)) { //UDP sollte alles auf einmal senden
        fprintf(stderr, "sendto: only %zd of %zu bytes sent\n", sent, sizeof(*req));
        return NULL;
    }

    // 2) Ein Intervall warten: entweder ACK kommt oder Slot endet
    fd_set rfds; //Menge von Sockets die auf lesbar geprüft werden
    FD_ZERO(&rfds); //Set leeren
    FD_SET(g_sock, &rfds); //die UDP-Socket hinzufügen

    struct timeval tv; //Timeout für select (Slot-Länge)
    long total_us = (long)GBN_TIMEOUT_INT_MS * 1000L;
    tv.tv_sec = total_us / 1000000L;
    tv.tv_usec = total_us % 1000000L;

    int rc;
    do {
        rc = select(g_sock + 1, &rfds, NULL, NULL, &tv);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0){
        perror("select");
        return NULL;
    }
    if (rc == 0){
        return NULL;
    }

    // 3) ACK liegt an: lesen
    static struct answer ans; 
    memset(&ans, 0, sizeof(ans)); //Antwort initialisieren

    ssize_t got = recvfrom(g_sock, &ans, sizeof(ans), 0, NULL, NULL); //ACK lesen
    if (got < 0) { //Fehler oder doch nichts da
        if (errno == EAGAIN || errno == EWOULDBLOCK) { //non-blocking: doch kein Paket
            return NULL;

        }
        perror("recvfrom");
        return NULL;
    }

    if((size_t)got != sizeof(ans)) { //falsche Länge -> kaputtes/anderes Paket
        fprintf(stderr, "recvfrom: wrong answer size %zd (expected %zu)\n", got,sizeof(ans));
        return NULL;
    }

    // 4) bis Slot-Ende idle: Restzeit aus select()
    if (tv.tv_sec > 0 || tv.tv_usec > 0){
        (void)select(0,NULL, NULL, NULL, &tv);
    }

    return &ans;

    /* TODO:
     *  - GBN-/ARQ-Sendealgorithmus implementieren
     *  - select() zur Realisierung der Sendeintervalle verwenden
     *  - bei Timeout Retransmissions auslösen
     *  - Antwortpaket (struct answer) bei Erfolg zurückgeben
     */

}

/* --------------------------------------------------------------- */
/*  Externe ARQ-API (Wrapper um doRequest)                         */
/* --------------------------------------------------------------- */

int arqSendHello(int winSize)
{
    struct request req; //Request-Paket anlegen (lokal auf dem Stack)
    memset(&req, 0, sizeof(req)); //alles auf 0, damit keine Zufallswerte drin sind

    req.ReqType = ReqHello; //HELLO-Pakettyp setzen
    req.FlNr = 0; //bei HELLO keine Nutzdatenlänge
    req.SeNr = 0; //bei HELLO keine Sequenznummer nötig

    if (winSize < 1) winSize = 1; //min Fenstergröße 
    if (winSize > GBN_MAX_WINDOW) winSize = GBN_MAX_WINDOW; //max Fenstergröße

    g_win = winSize; //Fenstergröße merken
    g_base = 0; //Start: erstes Paket
    g_next = 0; //Start:nächstes neues Paket
    g_inFlight = 0; //Start: nichts ausstehend

    memset(g_wvalid, 0, sizeof(g_wvalid)); //Ringpuffer leeren

    int windowFull = 0; //Ausgabeparameter: Fenster voll?
    int retransmission = 0; //Ausgabeparameter: Retransmission passiert?

    struct answer *ans = doRequest(&req, g_win, &windowFull, &retransmission); //HELLO senden
    if (ans == NULL) { //NULL = Fehler/keine Antwort
        return 1;
    }
    if (ans->AnswType == AnswHello || ans->AnswType == AnswOk) return 0;
    return 1;

    /* TODO:
     *  - struct request für Hello vorbereiten (ReqType = ReqHello)
     *  - Sequenznummern- und Fensterzustand initialisieren
     *  - doRequest(...) aufrufen
     *  - Antwort auswerten (AnswHello / AnswOk)
     *  - bei Fehler einen Wert != 0 zurückgeben
     */
}

int arqSendData(const struct app_unit *app, int winSize)
{
    (void)app;
    (void)winSize;

    /* TODO:
     *  - aus app eine struct request mit ReqType = ReqData erzeugen:
     *       * FlNr = app->len (ggf. begrenzen auf PufferSize)
     *       * SeNr = laufende Paketnummer (z.B. statischer Zähler)
     *       * Nutzdaten kopieren
     *  - doRequest(...) aufrufen
     *  - Antwort auswerten (AnswOk/AnswWarn/AnswErr)
	 *  - bei bufferFull oder retransmission doRequest mit unverändertem req erneut aufrufen
     *  - bei Fehler einen Wert != 0 zurückgeben
     */
    return 0;
}

int arqSendClose(int winSize)
{
    (void)winSize;

    /* TODO:
     *  - struct request mit ReqClose vorbereiten
     *  - doRequest(...) aufrufen
     *  - Antwort auswerten
     *  - bei Fehler einen Wert != 0 zurückgeben
     */
    return 0;
}
