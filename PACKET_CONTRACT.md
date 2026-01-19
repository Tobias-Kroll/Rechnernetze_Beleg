# PACKET_CONTRACT.md

Dieses Dokument fixiert das gemeinsame Paketformat und die verbindlichen Protokollregeln für unsere Implementierung

## 1 Pflichtregeln (Semantik)

### 1.1 Transport & Struktur
- Transport: UDP über IPv6
- ARQ: Go-Back-N, Fenstergröße 1–10
- Keine Threads, genau ein Socket pro Instanz
- Programmstruktur: client.c/server.c (File-IO) + clientSy.c/serverSy.c (Protokoll/Socket)

### 1.2 ACK-Semantik
- Der Empfänger führt expectedSeq.
- Der Empfänger sendet als ACK immer die nächste erwartete Sequenznummer:
  - ACK = expectedSeq bedeutet: Alle Pakete mit Sequenznummer < expectedSeq sind korrekt angekommen; als nächstes erwarte ich expectedSeq

### 1.3 Empfänger-Verhalten
- Bei DATA(seq):
  - Wenn seq == expectedSeq: Daten akzeptieren/schreiben; expectedSeq++
  - Sonst: verwerfen (drop)
- Der Empfänger sendet nach jedem relevanten Empfang ein ACK wie oben.

### 1.4 Sender-Verhalten (zeitsynchron, Slot-basiert)
- Der Sender arbeitet in Zeitslots
- Pro Slot wird maximal ein neues Datenpaket gesendet
- Timeout-/Retransmit hat Vorrang vor dem Senden eines neuen Pakets
- Bei Timeout: Go-Back-N — das entsprechende Paket und alle danach unbestätigten Pakete erneut senden.
- ACKs außerhalb des aktuellen Fensters werden verworfen

### 1.5 Verbindungssteuerung
- Vor Daten: HELLO (muss bestätigt werden)
- Nach Daten: CLOSE (muss bestätigt werden)
- Verlustfälle (HELLO/CLOSE/ACK) müssen durch Wiederholen bis Bestätigung behandelt werden

## 2 Paketformat (Designentscheidung: fester Header + optionale Payload)

### 2.1 Pakettypen
- `HELLO`
- `DATA`
- `ACK`
- `CLOSE`

### 2.2 Header-Felder (wire-format)

Request:

| Feld     | Bedeutung                              |
|----------|----------------------------------------|
| ReqType  | 'H' = Hello, 'D' = Data, 'C' = Close   |
| FlNr     | Nutzdatenlänge in Bytes                |
| SeNr     | Sequenznummer (Paketnummer: 0,1,2,...  |
| name[512]| Payload                                |

Answer:

| Feld       | Bedeutung                                 |
|------------|-------------------------------------------|
| AnswType   | 'H' = Hello ACK, 'O' = Ok ACK, 'W' = 0xFF |
| SeNo       | next expected (bei AnswOk)                |

Payload ist nur bei DATA vorhanden und enthält die zu übertragenden Nutzdaten (z. B. eine Textzeile).

### 2.3 Byteorder
- Alle Mehrbyte-Felder werden in Network Byte Order übertragen:
  - Sender: htons/htonl
  - Empfänger: ntohs/ntohl

### 2.4 Maximale Payload-Größe
- Wir definieren MAX_PAYLOAD so, dass UDP-Pakete typischerweise ohne Fragmentierung übertragen werden können.
- MAX_PAYLOAD = 512 bytes

## 3 Sequenznummern-Regeln

- Startwert der DATA-Sequenznummer: 0
- Der Sender hält:
  - base = Sequenznummer des ältesten unbestätigten Pakets
  - nextSeq = nächste neue Sequenznummer, die gesendet werden darf
- Ein ACK mit Wert A bestätigt kumulativ alle Sequenzen < A und erlaubt base auf A zu setzen
