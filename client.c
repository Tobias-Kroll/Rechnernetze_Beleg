#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "data.h"
#include "config.h"
#include "clientSy.h"

/* usage-Ausgabe */
static void usage(const char *progName)
{
    fprintf(stderr, "Usage: %s -a <server> -p <port> -f <file> -w <window>\n", progName);
    fprintf(stderr, "       -a <server> : Server-Adresse (Default: %s)\n",
            (DEFAULT_SERVER == NULL) ? "loopback" : DEFAULT_SERVER);
    fprintf(stderr, "       -p <port>   : Server-Port (Default: %s)\n", DEFAULT_PORT);
    fprintf(stderr, "       -f <file>   : Eingabedatei\n");
    fprintf(stderr, "       -w <window> : Fenstergröße (1..10)\n");
    exit(EXIT_FAILURE);
}

/* Eine Zeile aus Datei in app_unit einlesen (Begrenzung BufferSize).
 *
 * Signatur ist vorgegeben und soll beibehalten werden.
 *
 * - Rückgabewerte:
 *        > 1 : es wurden Nutzdaten gelesen
 *        > 0 : EOF (keine Daten mehr)
 *        < 0 : Fehler (z.B. ferror(f) != 0)
 */
static int readAppUnit(struct app_unit *app, FILE *f)
{
    /* TODO: Eine Zeile einlesen */
    char *result;
    
    if (!app || !f) {
        return -1;
    }
    
    result = fgets((char *)app->data, sizeof(app->data), f);
    
    if (result == NULL) {
        if (ferror(f)) {
            return -1;  /* Fehler */
        }
        return 0;  /* EOF */
    }
    
    /* Länge berechnen (ohne Newline, wenn vorhanden) */
    app->len = strlen((char *)app->data);
    if (app->len > 0 && app->data[app->len - 1] == '\n') {
        app->len--;
        app->data[app->len] = '\0';
    }
    
    /* Leere Zeilen (nur Newline) überspringen */
    if (app->len == 0) {
        return readAppUnit(app, f);  /* Nächste Zeile lesen */
    }
    
    return 1;  /* Daten gelesen */
}

int main(int argc, char *argv[])
{
    const char *server     = DEFAULT_SERVER;
    const char *filename   = NULL;
    const char *port       = DEFAULT_PORT;
    const char *windowSize = "1";

    FILE *fp = NULL;
    long i;

    /* Kommandozeilenparameter auswerten */
    if (argc > 1) {
        for (i = 1; i < argc; i++) {
            if (((argv[i][0] == '-') || (argv[i][0] == '/')) &&
                (argv[i][1] != 0) && (argv[i][2] == 0)) {

                switch (tolower((unsigned char)argv[i][1])) {

                case 'a': /* Server-Adresse */
                    if (argv[i + 1] && argv[i + 1][0] != '-') {
                        server = argv[++i];
                        break;
                    }
                    usage(argv[0]);
                    break;

                case 'p': /* Server-Port */
                    if (argv[i + 1] && argv[i + 1][0] != '-') {
                        port = argv[++i];
                        break;
                    }
                    usage(argv[0]);
                    break;

                case 'f': /* Eingabedatei */
                    if (argv[i + 1] && argv[i + 1][0] != '-') {
                        filename = argv[++i];
                        break;
                    }
                    usage(argv[0]);
                    break;

                case 'w': /* Fenstergröße */
                    if (argv[i + 1] && argv[i + 1][0] != '-') {
                        windowSize = argv[++i];
                        break;
                    }
                    usage(argv[0]);
                    break;

                default:
                    usage(argv[0]);
                    break;
                }
            } else {
                usage(argv[0]);
            }
        }
    }

    if (!filename) {
        usage(argv[0]);
    }

    /* TODO:
     *   - Datei filename zum Lesen öffnen (z.B. fopen)
     *   - bei Fehler: perror / Fehlermeldung und EXIT_FAILURE
     *   - bei Erfolg: FILE* in fp ablegen
     */
    fp = fopen(filename, "r");
    if (!fp) {
        perror("fopen");
        fprintf(stderr, "Client: failed to open file '%s'.\n", filename);
        return EXIT_FAILURE;
    }

    printf("Client: sending file '%s'\n", filename);

    /* ARQ-Client initialisieren */
    initClient((char *)server, port);

    /* Hello/Verbindungsaufbau */
    if (arqSendHello(atoi(windowSize)) != 0) {
        fprintf(stderr, "Client: Hello failed, aborting.\n");
        /* TODO: Datei ggf. schließen, falls sie bereits geöffnet wurde */
        if (fp) {
            fclose(fp);
        }
        closeClient();
        return EXIT_FAILURE;
    }

    /* Datei -> zeilenweise lesen und jede Zeile als app_unit an 
	 * arqSendData() übergeben 
     *
     *   - Fehlerfall (readAppUnit(..) < 0) behandeln
     */
    {
        struct app_unit app;
        int readResult;
        
        while ((readResult = readAppUnit(&app, fp)) > 0) {
            if (arqSendData(&app, atoi(windowSize)) != 0) {
                fprintf(stderr, "Client: error while sending data.\n");
                break;
            }
        }
        
        if (readResult < 0) {
            fprintf(stderr, "Client: error reading file.\n");
        }
    }

    /* Close / Verbindungsabbau */
    if (arqSendClose(atoi(windowSize)) != 0) {
        fprintf(stderr, "Client: error while sending close.\n");
    }

    /* TODO:
     *   - geöffnete Datei wieder schließen
     */
    if (fp) {
        fclose(fp);
    }
    closeClient();

    return EXIT_SUCCESS;
}
