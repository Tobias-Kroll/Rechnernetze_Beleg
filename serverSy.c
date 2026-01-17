/* serverSy.c - UDP + ARQ/Go-Back-N-Server mit Loss-Simulation
 *
 * Schichten:
 *   - SAP-Schicht (UDP): initServer, getRequest, sendAnswer, exitServer
 *   - ARQ-Schicht: processRequest(), arqServerLoop()
 *
 * Die Anwendung (Datei öffnen/schreiben/schließen) wird über Callbacks
 * aus server.c angebunden:
 *   appStartFn  appStartTransfer
 *   appWriteFn  appWriteData
 *   appEndFn    appEndTransfer
 *
 * WICHTIG:
 *   - Dateiname und Funktionssignaturen in serverSy.h sollen
 *     unverändert beibehalten werden.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "data.h"
#include "config.h"
#include "serverSy.h"

/* Globale Variablen für die SAP-Schicht */
static int server_socket = -1;                    /* UDP/IPv6 Socket-Deskriptor */
static struct sockaddr_storage client_addr;      /* zuletzt verbundener Client */
static socklen_t client_addr_len;                /* Länge der Client-Adresse */

/* --------------------------------------------------------------- */
/*  SAP-Schicht (UDP)                                              */
/* --------------------------------------------------------------- */

/**
 * initServer: Initialisiert den UDP/IPv6-Server
 *   - Erstellt einen UDP/IPv6-Socket
 *   - Bindet ihn an alle lokalen Interfaces auf dem angegebenen Port
 *   - Speichert den Socket-Deskriptor
 *
 * Rückgabe: 0 bei Erfolg, <0 bei Fehler
 */
int initServer(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int ret;
    
    if (!port) {
        fprintf(stderr, "initServer: port is NULL\n");
        return -1;
    }

    /* getaddrinfo setup: IPv6, UDP, Servern für Bind */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = PF_INET6;           /* IPv6 */
    hints.ai_socktype = SOCK_DGRAM;         /* UDP */
    hints.ai_flags    = AI_PASSIVE;         /* für bind() */

    /* Adressinformation für Port ermitteln */
    ret = getaddrinfo(NULL, port, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    /* Socket erstellen */
    server_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_socket < 0) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }

    /* Socket an lokale Adresse binden */
    if (bind(server_socket, res->ai_addr, res->ai_addrlen) < 0) {
        perror("bind");
        close(server_socket);
        server_socket = -1;
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    printf("[Server] Socket initialized on port %s\n", port);
    return 0;
}

/**
 * getRequest: Liest ein Request-Paket vom UDP-Socket
 *   - Blockierend: wartet auf eingehendes Paket
 *   - Speichert die Client-Adresse für sendAnswer()
 *
 * Rückgabe: Zeiger auf struct request, oder NULL bei Fehler
 */
struct request *getRequest(void)
{
    static struct request req;
    ssize_t n;

    if (server_socket < 0) {
        fprintf(stderr, "getRequest: server not initialized\n");
        return NULL;
    }

    /* Client-Adresse initialisieren */
    client_addr_len = sizeof(client_addr);

    /* Paket vom Socket lesen */
    n = recvfrom(server_socket, &req, sizeof(req), 0,
                 (struct sockaddr *)&client_addr, &client_addr_len);
    
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recvfrom");
        }
        return NULL;
    }

    if (n < (ssize_t)sizeof(struct request)) {
        fprintf(stderr, "getRequest: packet too small (%zd bytes)\n", n);
        return NULL;
    }

    printf("[Server] Received packet: Type=%c, SeNr=%lu, FlNr=%lu\n",
           req.ReqType, req.SeNr, req.FlNr);

    return &req;
}

/**
 * sendAnswer: Sendet eine Antwort zum Client
 *   - Verwendet die zuletzt empfangene Client-Adresse
 *   - Simuliert ACK-Verluste falls nötig
 *
 * Rückgabe: 0 bei Erfolg, <0 bei Fehler
 */
int sendAnswer(struct answer *answerPtr)
{
    ssize_t n;

    if (!answerPtr) {
        fprintf(stderr, "sendAnswer: answer is NULL\n");
        return -1;
    }

    if (server_socket < 0) {
        fprintf(stderr, "sendAnswer: server not initialized\n");
        return -1;
    }

    n = sendto(server_socket, answerPtr, sizeof(struct answer), 0,
               (struct sockaddr *)&client_addr, client_addr_len);

    if (n < 0) {
        perror("sendto");
        return -1;
    }

    printf("[Server] Sent answer: Type=%c, SeNo=%lu (next expected)\n",
           answerPtr->AnswType, answerPtr->SeNo);

    return 0;
}

/**
 * exitServer: Beendet den Server
 *   - Schließt den Socket
 *   - Setzt globale Variablen zurück
 */
int exitServer(void)
{
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    printf("[Server] Server shutdown\n");
    return 0;
}

/* --------------------------------------------------------------- */
/*  ARQ-/GBN-Logik (Empfänger)                                     */
/* --------------------------------------------------------------- */

/* Globale Zustandsvariablen für die ARQ-Logik */
static unsigned long nextExpected = 0;  /* Nächst erwartete Sequenznummer */
static int session_active = 0;          /* Session aktiv? (nach HELLO) */

/* Globale Callback-Funktionszeiger */
static appStartFn g_appStart = NULL;
static appWriteFn g_appWrite = NULL;
static appEndFn   g_appEnd   = NULL;

/*
 * Hilfsfunktion: Simuliert Paketverlust
 *   - Gibt 1 zurück (verwerfen), wenn rand() < loss_rate * RAND_MAX
 *   - Ansonsten 0 (behalten)
 */
static int simulate_loss(double loss_rate)
{
    if (loss_rate <= 0.0) return 0;
    if (loss_rate >= 1.0) return 1;
    return (rand() < (int)(loss_rate * RAND_MAX)) ? 1 : 0;
}

/*
 * processRequest:
 *  - nimmt ein Request-Paket entgegen
 *  - führt die ARQ-/GBN-Empfangslogik aus
 *  - erzeugt eine passende Antwort (ACK/Fehler)
 *
 *   ReqHello:
 *     - Sequenznummernzustand initialisieren (nextExpected = 0)
 *     - Anwendung per appStartFn informieren
 *     - eine passende Antwort (AnswHello) eintragen
 *
 *   ReqData:
 *     - Sequenznummer prüfen
 *     - nur bei ReqType == ReqData AND SeNr == nextExpected: 
 *       * Nutzdaten an appWriteFn übergeben
 *       * nextExpected inkrementieren
 *     - ggf. ACK (AnswOk) mit nextExpected senden
 *       
 *   ReqClose:
 *     - appEndFn aufrufen
 *     - Abschluss-ACK senden
 *
 * lossReq:
 *   - simulierte Paketverlustrate für Requests (0.0..1.0)
 *     (z.B. über Zufallszahlvergleich ein Paket "fallen lassen")
 *
 * Rückgabewert:
 *   - Zeiger auf ausgefüllte Antwortstruktur (answPtr)
 *   - NULL, wenn das Request-Paket vollständig verworfen wurde
 */
static struct answer *processRequest(struct request *reqPtr,
                                     struct answer *answPtr,
                                     double lossReq)
{
    if (!reqPtr || !answPtr) {
        fprintf(stderr, "processRequest: invalid pointers\n");
        return NULL;
    }

    /* Paketverlust auf Sender-Seite simulieren */
    if (simulate_loss(lossReq)) {
        printf("[Server] Request packet DROPPED (simulated loss)\n");
        return NULL;  /* Paket verworfen, kein ACK */
    }

    /* Paketverarbeitung nach Typ */
    switch (reqPtr->ReqType) {

    case ReqHello:
        printf("[Server] HELLO received\n");
        
        /* Session initialisieren */
        nextExpected = 0;
        session_active = 1;
        
        /* Anwendung starten */
        if (g_appStart && g_appStart() < 0) {
            fprintf(stderr, "[Server] appStart failed\n");
            answPtr->AnswType = AnswErr;
            answPtr->ErrNo = ERR_FILE_ERROR;
        } else {
            answPtr->AnswType = AnswHello;
            answPtr->SeNo = 0;
        }
        break;

    case ReqData:
        printf("[Server] DATA received: SeNr=%lu, FlNr=%lu, nextExpected=%lu\n",
               reqPtr->SeNr, reqPtr->FlNr, nextExpected);

        if (!session_active) {
            printf("[Server] DATA empfangen ohne aktive Session\n");
            answPtr->AnswType = AnswErr;
            answPtr->ErrNo = ERR_ILLEGAL_REQUEST;
            answPtr->SeNo = nextExpected;
        } else if (reqPtr->SeNr == nextExpected) {
            /* *** RECEIVER-REGEL: Nur erwartete Sequenznummer akzeptieren *** */
            printf("[Server] Accepting DATA with correct SeNr=%lu\n", reqPtr->SeNr);
            
            /* Nutzdaten an Anwendung übergeben */
            if (g_appWrite) {
                if (g_appWrite(reqPtr->name, reqPtr->FlNr) < 0) {
                    fprintf(stderr, "[Server] appWrite failed\n");
                    answPtr->AnswType = AnswErr;
                    answPtr->ErrNo = ERR_FILE_ERROR;
                    answPtr->SeNo = nextExpected;
                } else {
                    /* Erfolgreich geschrieben -> nächste Seq erwarten */
                    nextExpected++;
                    answPtr->AnswType = AnswOk;
                    answPtr->SeNo = nextExpected;  /* Kumulativ */
                }
            } else {
                nextExpected++;
                answPtr->AnswType = AnswOk;
                answPtr->SeNo = nextExpected;
            }
        } else {
            /* DROPPEN: Out-of-order Paket */
            printf("[Server] OUT-OF-ORDER: received SeNr=%lu, expected %lu -> DROPPED\n",
                   reqPtr->SeNr, nextExpected);
            /* Aber trotzdem ACK mit aktuell erwarteter Sequenznummer senden */
            answPtr->AnswType = AnswOk;
            answPtr->SeNo = nextExpected;  /* Kumulativ */
        }
        break;

    case ReqClose:
        printf("[Server] CLOSE received\n");
        
        if (!session_active) {
            printf("[Server] CLOSE ohne aktive Session\n");
            answPtr->AnswType = AnswErr;
            answPtr->ErrNo = ERR_ILLEGAL_REQUEST;
        } else {
            /* Anwendung beenden */
            if (g_appEnd) {
                g_appEnd();
            }
            session_active = 0;
            answPtr->AnswType = AnswOk;
            answPtr->SeNo = nextExpected;  /* Finale Seq */
        }
        break;

    default:
        printf("[Server] Unknown ReqType: %c\n", reqPtr->ReqType);
        answPtr->AnswType = AnswErr;
        answPtr->ErrNo = ERR_ILLEGAL_REQUEST;
        break;
    }

    return answPtr;
}

/* --------------------------------------------------------------- */
/*  ARQ-Server-Hauptschleife                                       */
/* --------------------------------------------------------------- */

int arqServerLoop(const char *port,
                  double lossReq,
                  double lossAck,
                  appStartFn appStart,
                  appWriteFn appWrite,
                  appEndFn appEnd)
{
    struct request *reqPtr;
    struct answer answer;
    int ret;

    /* Callbacks speichern */
    g_appStart = appStart;
    g_appWrite = appWrite;
    g_appEnd = appEnd;

    /* Server initialisieren */
    if (initServer(port) < 0) {
        fprintf(stderr, "[Server] Failed to initialize server\n");
        return -1;
    }

    printf("[Server] Starting ARQ loop (lossReq=%.2f, lossAck=%.2f)\n", lossReq, lossAck);

    /* Hauptschleife: Pakete empfangen und verarbeiten */
    while (1) {
        /* Paket vom Client empfangen */
        reqPtr = getRequest();
        if (!reqPtr) {
            /* Timeout oder Fehler - weitermachen */
            continue;
        }

        /* ARQ-Logik: Request verarbeiten */
        if (processRequest(reqPtr, &answer, lossReq) == NULL) {
            /* Paket wurde wegen simuliertem Verlust verworfen */
            continue;
        }

        /* ACK-Verlust simulieren */
        if (simulate_loss(lossAck)) {
            printf("[Server] ACK DROPPED (simulated loss) for SeNo=%lu\n", answer.SeNo);
            continue;  /* ACK nicht senden */
        }

        /* ACK senden */
        ret = sendAnswer(&answer);
        if (ret < 0) {
            fprintf(stderr, "[Server] Failed to send answer\n");
            /* Schleife fortsetzen - bei ernstlichen Fehlern könnte man auch abbrechen */
        }

        /* Wenn CLOSE mit Erfolg abgeschlossen: Session beenden */
        if (!session_active && answer.AnswType == AnswOk) {
            printf("[Server] Session closed, exiting loop\n");
            break;  /* Schleife beenden */
        }
    }

    /* Server cleanup */
    exitServer();
    printf("[Server] arqServerLoop terminated\n");
    return 0;
}
