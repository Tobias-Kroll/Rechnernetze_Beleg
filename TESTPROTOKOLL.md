# TESTPROTOKOLL.md — QA / Reproduzierbare Tests

Ausfüllbares Testprotokoll gemäß Aufgabenanforderung:
- Fenstergröße 1 und >1
- fehlerfrei + Fehlerfälle (ACK-Verlust, Paketverlust)
- HELLO/CLOSE Fehlerfälle
- Tests müssen reproduzierbar sein (Commands + erwartetes Ergebnis)

## 1 Prüfmethode (für jeden Test)
1) Server starten (Output-Datei festlegen)
2) Client starten (Input-Datei + Fenstergröße)
3) Nach Abschluss: **Dateivergleich**
   - `diff input.txt output.txt`
   - keine Ausgabe = **PASS**
4) Logs prüfen: Retransmits / ACK-Werte / Sliding sind nachvollziehbar

## 2 Testmatrix (auszufüllen)

> Hinweis: Spalten „Server-Command“/„Client-Command“ werden final ergänzt, sobald klar ist,
> wie die Argumente (Fenstergröße, Loss-Simulation) im Code genau heißen.

| ID | Window  | PacketLoss | AckLoss             | Szenario            | Server-Command | Client-Command | Erwartung                                       | Ergebnis | Notizen |
|---:|:-------:|:----------:|:-------------------:|---------------------|----------------|----------------|-------------------------------------------------|:--------:|---------|
| T1 | 1       | 0%         | 0%                  | Stop-and-Wait Basis |                |                | Output == Input                                 |          |         |
| T2 | 5       | 0%         | 0%                  | GBN fehlerfrei      |                |                | Sliding sichtbar; Output == Input               |          |         |
| T3 | 5       | 30%        | 0%                  | Paketverlust        |                |                | Go-Back-N Retransmit ab `base`; Output == Input |          |         |
| T4 | 5       | 0%         | 30%                 | ACK-Verlust         |                |                | Retransmits möglich; Output == Input            |          |         |
| T5 | 5       | 0%         | 0%                  | HELLO-ACK verloren  |                |                | HELLO wird wiederholt bis bestätigt             |          |         |
| T6 | 5       | 0%         | 0%                  | CLOSE-ACK verloren  |                |                | CLOSE wird wiederholt bis bestätigt             |          |         |

## 3 Akzeptanzkriterien (PASS)
Ein Test gilt als PASS, wenn:
- Übertragung endet korrekt (kein Hängen)
- Output-Datei ist identisch zur Input-Datei (`diff` leer)
- Verhalten entspricht Semantik (kumulative ACKs „next expected“, Out-of-order drop, Go-Back-N bei Timeout)