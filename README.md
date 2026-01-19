# README.md

## Zweck
Implementierung eines ARQ-Protokolls (Go-Back-N) zur **zeilenweisen** Übertragung einer Textdatei über **UDP/IPv6**.
Die Code-Struktur folgt dem Template:
- `client.c` / `server.c`: Datei-Handling
- `clientSy.c` / `serverSy.c`: Protokoll, Socket, ARQ-Logik
Ohne Threads, genau ein Socket pro Instanz.

## Build (Linux)
```bash
make
```

## Ru
```bash

# Server
./server -p <port> -f <outfile> [-r <lossReq>] [-a <lossAck>]

# Client
./client -a <server> -p <port> -f <file> -w <window>

## Doku / QA Artefakte
- `PACKET_CONTRACT.md` — gemeinsames Paketformat + Semantik + Meeting-Checkliste
- `TESTPROTOKOLL.md` — Testmatrix (reproduzierbar, PASS/FAIL)
- `diagramme/` — Weg-Zeit-Diagramme + Zustandsdiagramme (wird ergänzt)

## Definition of Done
- Kompiliert und läuft unter Linux
- Fenstergröße 1 und >1 getestet
- Loss-Szenarien: ACK-Verlust, Paketverlust (per Argument) getestet
- HELLO/CLOSE Fehlerfälle getestet
- Diagramme erstellt und in `diagramme/` abgelegt

