# Rechnernetze – APL Übersicht & Lernzettel

## Zweck
Implementierung eines **zuverlässigen Übertragungsprotokolls (ARQ, Go-Back-N)** zur zeilenweisen Übertragung einer Textdatei über **UDPv6/IPv6** zwischen Client und Server.

Die Zuverlässigkeit (Reihenfolge, ACKs, Wiederholungen) wird **nicht durch TCP**, sondern **selbst implementiert**.

---

## 1. Allgemeines

- Client- und Serverprogramm in **C**
- Übertragung einer **Textdatei zeilenweise**
- Transport über **UDPv6 / IPv6**
- Zuverlässigkeit durch **ARQ-Protokoll (Go-Back-N)**

### Programmargumente (Client & Server)
- Dateiname (Input / Output)
- IPv6-Adresse
- Port
- Fenstergröße (1–10, optional >1)

---

## 2. Server – Empfangslogik (Empfänger)

### Grundprinzip
- Jedes Paket besitzt eine **Sequenznummer**
- Server verwaltet die Variable `expected_seq` (nächste erwartete Nummer)

### Paketempfang
- **Falls `seq == expected_seq`:**
  - Paket ist korrekt
  - Daten an Anwendung weitergeben (in Datei schreiben)
  - `expected_seq++`

- **Falls `seq != expected_seq`:**
  - Paket ist außerhalb der Reihenfolge
  - Paket wird **gedroppt**

### ACK-Regeln
- Nach **JEDEM empfangenen Paket** wird ein ACK gesendet
- ACK enthält die **nächste erwartete Sequenznummer (`expected_seq`)**
- ACKs sind **kumulativ**

### Einschränkungen
- ❌ Keine Pufferung auf Empfängerseite
- ❌ Keine Sortierung
- ❌ Out-of-order-Pakete werden verworfen

---

## 3. Client – Senderlogik (Go-Back-N)

### Zeitmodell
- Feste **Zeitschlitze** (z. B. 100 ms)
- Pro Zeitschlitz **maximal EIN neues Paket**

### Sendefenster
- Implementiert als **Ringpuffer**
- Enthält:
  - bereits gesendete
  - aber noch **nicht quittierte** Pakete
- Fenstergröße: **1–10**

### Senden
- Fenster hat Platz → **neues Paket senden**
- Timer abgelaufen → **Wiederholung hat Vorrang**
  - Resend ab dem **ältesten unquittierten Paket**

### Timer
- Timer läuft in Vielfachen der Zeitschlitze
- Pro Intervall → **Timer dekrementieren**
- Timer = 0 → Timeout

### Warten (`select()`)
- Sender wartet:
  - bis Intervall endet **oder**
  - bis **EIN ACK** eintrifft
- Kommt ACK vor Intervallende:
  - trotzdem bis Intervallende warten

### ACK-Behandlung
- ACK ist **kumulativ**
- Prüfen:
  - ACK-Seq liegt im Sendefenster → Fenster nach vorne schieben
  - sonst → ACK verwerfen

### Timeout-Fall
- Kein ACK + Timer abgelaufen:
  - erneutes Senden:
    - des betroffenen Pakets
    - **und aller danach gesendeten, noch unquittierten Pakete**
- → **Go-Back-N**

---

## 4. Verbindungsaufbau & -abbau (trotz UDP)

### Aufbau
- Client sendet **HELLO-Paket**
- Client wartet auf ACK
- Erst nach HELLO-ACK → DATA-Pakete erlaubt
- Kein ACK → HELLO erneut senden

### Datenübertragung
- DATA-Pakete mit ARQ (Go-Back-N)
- ACKs sind kumulativ
- Paket- und ACK-Verluste möglich (Simulation)

### Abbau
- Nach letzter Datenzeile → Client sendet **CLOSE**
- Client wartet auf ACK
- Kein ACK → CLOSE erneut senden
- Nach CLOSE-ACK → Verbindung sauber beendet

### Fehlerfälle
- HELLO / CLOSE / ACK geht verloren → erneut senden
- DATA ohne HELLO → ignorieren

---

## 5. Code-Struktur (Pflicht)

+---------------------- Anwendungsebene ----------------------+
|                                                            |
|  client.c                     server.c                    |
|  --------                     --------                    |
|  - main()                     - main()                    |
|  - Argumente                  - Argumente                 |
|  - Datei lesen                - Datei schreiben           |
|  - Aufruf ARQ                 - Aufruf ARQ                |
|                                                            |
+---------------------------|--------------------------------+
                            |
                            v
+------------------ ARQ + Transportebene --------------------+
|                                                            |
|  clientSy.c                  serverSy.c                   |
|  ----------                  ----------                   |
|  - UDPv6 Socket              - UDPv6 Socket               |
|  - Paketformate              - expected_seq               |
|  - Sendefenster (Ring)       - Drop out-of-order          |
|  - Timer + select()          - ACK + Zustände             |
|  - Go-Back-N                 - Zustandsmaschine           |
|                                                            |
+------------------------------------------------------------+



---

## 6. Technische Vorgaben
- ❌ Keine Threads
- ✅ Genau **1 Socket pro Programm**
- ✅ Warten mit `recvfrom()` / `select()`

---

## 7. Dokumentation (Abgabe)

### 1) Designentscheidungen
- Paketformat
- Sequenznummern
- Timer-Strategie
- Fenstergröße
- Verlustsimulation

### 2) Weg-Zeit-Diagramme
- Fenstergröße = 1
- Fenstergröße > 1
- Fehlerfreier Fall
- Fehlerbehafteter Fall (Paket-/ACK-Verlust)

### 3) Zustandsdiagramme
- **Sender** und **Empfänger**
- Enthalten:
  - Variablen (`base`, `next_seq`, `expected_seq`)
  - Bedingungen (`ACK im Fenster`, `Timeout`)
  - Aktionen (`send()`, `resend()`, Fenster verschieben)

---

## 8. Wichtige Begriffe (Vokabeln)

- **Client**: sendendes Programm, steuert Fenster, Timer, Resends
- **Server**: empfangendes Programm, prüft Reihenfolge, schreibt Datei
- **UDPv6**: UDP über IPv6, ohne Zuverlässigkeit
- **ARQ**: Automatic Repeat reQuest
- **Go-Back-N**: ARQ-Variante mit Resend ab Fehler
- **Sequenznummer (`seq`)**: Nummer im Paket
- **expected_seq**: nächste erwartete Sequenznummer (Server)
- **ACK**: Bestätigung empfangener Pakete
- **kumulativ**: ein ACK bestätigt mehrere Pakete
- **Sendefenster**: noch nicht bestätigte Pakete
- **Ringpuffer**: Datenstruktur für das Sendefenster
- **select()**: Warten auf ACK oder Timeout
- **HELLO / DATA / CLOSE**: Pakettypen

---

## 9. Person 2  – ServerSy (Receiver-ARQ)

### Aufgaben
- Implementierung der Empfängerseite des ARQ-Protokolls
- UDPv6 als Transport
- Genau ein Socket, keine Threads
- Paketempfang mit `recvfrom()`

### Zustandsmaschine
- `WAIT_HELLO`
- `WAIT_DATA`
- `WAIT_CLOSE`

### Logging
- Empfangene Pakete
- Gedroppte Pakete
- Gesendete ACKs
- Zustandswechsel
