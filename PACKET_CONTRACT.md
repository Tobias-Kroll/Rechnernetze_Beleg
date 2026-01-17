# PACKET_CONTRACT.md

Dieses Dokument fixiert das gemeinsame Paketformat und die verbindlichen Protokollregeln für unsere Implementierung.
Es enthält sowohl Pflichtvorgaben aus der Aufgabenstellung (Semantik/Verhalten) als auch explizite Designentscheidungen
(z. B. Feldgrößen im Header), damit Client/Server kompatibel sind.

## 1 Pflichtregeln (Semantik)

### 1.1 Transport & Struktur
- Transport: **UDP über IPv6**
- ARQ: **Go-Back-N**, Fenstergröße **1–10** (per Argument)
- **Keine Threads**, **genau ein Socket** pro Instanz
- Programmstruktur gemäß Template: `client.c/server.c` (File-IO) + `clientSy.c/serverSy.c` (Protokoll/Socket)

### 1.2 ACK-Semantik (kumulativ, „next expected“)
- Der Empfänger führt `expectedSeq`.
- Der Empfänger sendet als ACK immer **die nächste erwartete Sequenznummer**:
  - `ACK = expectedSeq` bedeutet: „Alle Pakete mit Sequenznummer < expectedSeq sind korrekt angekommen; als nächstes erwarte ich expectedSeq.“

### 1.3 Empfänger-Verhalten (kein Out-of-Order-Puffer)
- Bei DATA(seq):
  - Wenn `seq == expectedSeq`: Daten akzeptieren/schreiben; `expectedSeq++`
  - Sonst: **verwerfen (drop)** (kein Puffern)
- Der Empfänger sendet nach jedem relevanten Empfang ein ACK wie oben.

### 1.4 Sender-Verhalten (zeitsynchron, Slot-basiert)
- Der Sender arbeitet in Zeitslots (Slotdauer als Parameter/Define).
- Pro Slot wird **maximal ein neues** Datenpaket gesendet (wenn Fenster Platz hat).
- **Timeout-/Retransmit** hat Vorrang vor dem Senden eines neuen Pakets.
- Bei Timeout: Go-Back-N — **das entsprechende Paket und alle danach unbestätigten** Pakete erneut senden.
- ACKs außerhalb des aktuellen Fensters werden verworfen.

### 1.5 Verbindungssteuerung
- Vor Daten: **HELLO** (muss bestätigt werden)
- Nach Daten: **CLOSE** (muss bestätigt werden)
- Verlustfälle (HELLO/CLOSE/ACK) müssen durch Wiederholen bis Bestätigung behandelt werden.

## 2 Paketformat (Designentscheidung: fester Header + optionale Payload)

### 2.1 Pakettypen
- `HELLO`
- `DATA`
- `ACK`
- `CLOSE`

### 2.2 Header-Felder (wire-format)
Wir nutzen feste Breiten (stdint), um Plattformunabhängigkeit sicherzustellen.

| Feld   | Typ        | Bedeutung                                          |
|--------|------------|----------------------------------------------------|
| `type` | `uint8_t`  | Pakettyp (HELLO/DATA/ACK/CLOSE)                    |
| `seq`  | `uint32_t` | Sequenznummer (primär für DATA; bei HELLO/CLOSE 0) |
| `ack`  | `uint32_t` | ACK-Nummer („next expected“) — relevant für ACK    |
| `len`  | `uint16_t` | Länge der Payload in Bytes                         |

Payload ist **nur** bei DATA vorhanden und enthält die zu übertragenden Nutzdaten (z. B. eine Textzeile).

### 2.3 Byteorder
- Alle Mehrbyte-Felder werden in **Network Byte Order (big-endian)** übertragen:
  - Sender: `htons/htonl`
  - Empfänger: `ntohs/ntohl`

### 2.4 Maximale Payload-Größe
- Wir definieren `MAX_PAYLOAD` so, dass UDP-Pakete typischerweise **ohne Fragmentierung** übertragen werden können.
- Konkreter Wert (im Code als `#define`): **wird im Team final festgelegt** und hier nachgetragen, sobald entschieden.

## 3 Sequenznummern-Regeln

- **Startwert** der DATA-Sequenznummer: **0**
- Der Sender hält:
  - `base` = Sequenznummer des ältesten unbestätigten Pakets
  - `nextSeq` = nächste neue Sequenznummer, die gesendet werden darf
- Ein ACK mit Wert `A` bestätigt kumulativ alle Sequenzen `< A` und erlaubt `base` auf `A` zu setzen (wenn `A` im Fenster liegt).

## 4 Offene Team-Entscheidungen

Diese Punkte sind nicht als Semantik vorgeschrieben, müssen aber einheitlich festgelegt werden:

1) **Konkrete Kodierung von `type`** (z. B. 1..4)  
2) **MAX_PAYLOAD** (Zahl)  
3) **Slotdauer (ms)** und **Timeout in Slots** (als Vielfache der Slotdauer)