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

## Run (muss ergänzt werden)
```bash
# Server
./server <IPv6-Adresse> <Port> <Output-Datei> [Optionen...]

# Client
./client <IPv6-Adresse> <Port> <Input-Datei> <Window 1..10> [Optionen...]
```

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
