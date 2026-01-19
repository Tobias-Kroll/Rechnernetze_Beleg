# README.md

## Zweck
Implementierung eines ARQ-Protokolls (Go-Back-N) zur zeilenweisen Übertragung einer Textdatei über UDP/IPv6.
Die Code-Struktur folgt dem Template:
- `client.c` / `server.c`: Datei-Handling
- `clientSy.c` / `serverSy.c`: Protokoll, Socket, ARQ-Logik
Ohne Threads, genau ein Socket pro Instanz.

## Build (Linux)

make


## Run

# Server
./server -p <port> -f <outfile> -r <lossReq> -a <lossAck>

# Client
./client -a <server> -p <port> -f <file> -w <window>

## Dokumentation
- PACKET_CONTRACT.md — gemeinsames Paketformat + Semantik
- TESTFÄLLE.md — unsere ausgeführten Testes dokumentiert
- Diagramme — Weg-Zeit-Diagramme + Zustandsdiagramme

## Definition of Done
- Kompiliert und läuft unter Linux
- Fenstergröße 1 und >1 getestet
- Loss-Szenarien: ACK-Verlust, Paketverlust (per Argument) getestet
- HELLO/CLOSE Fehlerfälle getestet
- Diagramme erstellt und in `diagramme/` abgelegt
