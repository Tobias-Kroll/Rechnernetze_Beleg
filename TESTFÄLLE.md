TESTFÄLLE

T1

ID: T1
Window: 1
PacketLoss: 0%
AckLoss: 0%
Ziel: Nachweis, dass Fenstergröße 1 korrekt funktioniert (Sonderfall von Go-Back-N).
Server-Command: ./server -p 3331 -f out_T1.txt -r 0.0 -a 0.0 | tee logs/T1_server.logs
Client-Command: ./client -a ::1 -p 3331 -f input_small.txt -w 1

Erwartung: 
- Jedes DATA-Paket wird einzeln gesendet und bestätigt
- Keine Retransmits
- Output-Datei ist identisch zur Input-Date

Ergebnis: 
- diff input_small.txt out_T1.txt ist leer
- PASS

T2

ID: T2
Window: 5
PacketLoss: 0%
AckLoss: 0%
Ziel: Nachweis korrekter Go-Back-N-Übertragung mit Fenstergröße >1.
Server-Command: ./server -p 3332 -f out_T2.txt -r 0.0 -a 0.0 | tee logs/T2_server.log
Client-Command: ./client -a ::1 -p 3332 -f input_small.txt -w 5
Erwartung: 

- Mehrere DATA-Pakete gleichzeitig „in flight“
- Kumulative ACKs mit „next expected“
- Keine Paketverluste

Ergebnis: 

- diff input_small.txt out_T2.txt ist leer
- Logs zeigen fortlaufende SeNr und ACK SeNo
- PASS

T3

ID: T3
Window: 5
PacketLoss: 30%
AckLoss: 0%
Ziel: Nachweis, dass Paketverluste korrekt durch Go-Back-N behandelt werden.
Server-Command: ./server -p 3333 -f out_T3.txt -r 0.30 -a 0.0 | tee logs/T3_pktloss.log
Client-Command: ./client -a ::1 -p 3333 -f input_big.txt -w 5

Erwartung:

- Server verwirft zufällig DATA/HELLO/CLOSE-Pakete
- ACK bleibt auf nextExpected stehen
- Client retransmittiert alle Pakete ab base

Ergebnnis:

- diff input_big.txt out_T3.txt ist leer
- Log enthält Request packet DROPPED
- Wiederholte SeNr sichtbar (Retransmits)
- PASS


T4

ID: T4
Window: 5
PacketLoss: 0%
AckLoss: 30%
Ziel: Nachweis, dass ACK-Verluste korrekt behandelt werden.
Server-Command: ./server -p 3334 -f out_T4.txt -r 0.0 -a 0.30 | tee logs/T4_ackloss.log
Client-Command: ./client -a ::1 -p 3334 -f input_big.txt -w 5

Erwartung:

- Server verwirft zufällig ACKs
- Client erhält kein ACK → Timeout → Retransmit
- Empfänger erkennt Duplikate und schreibt nicht doppelt

Ergebnis:

- diff input_big.txt out_T4.txt ist leer
- Log enthält Answer packet DROPPED
- Retransmits sichtbar
- PASS

T5a

ID: T5a
Window: 5
PacketLoss: 0%
AckLoss: 70%
Ziel: Nachweis robuster Verbindungsaufnahme trotz ACK-Verlust.
Server-Command: ./server -p 3335 -f out_T5a.txt -r 0.0 -a 0.70 | tee logs/T5a_hello_ackloss.log
Client-Command: ./client -a ::1 -p 3335 -f input_small.txt -w 5

Erwartung:

- HELLO wird ggf. mehrfach gesendet
- Server antwortet mehrfach mit HELLO-ACK
- Verbindung wird korrekt aufgebaut

Ergebnis:

- diff input_small.txt out_T5a.txt → leer
- Mehrfache HELLO/ACK-Versuche im Log
- PASS

T5b

ID: T5b
Window: 5
PacketLoss: 70%
AckLoss: 0%
Ziel: Nachweis robuster Verbindungsaufnahme trotz Packetloss
Server-Command: ./server -p 3336 -f out_T5b.txt -r 0.70 -a 0.0 | tee logs/T5b_hello_reqloss.log
Client-Command: ./client -a ::1 -p 3335 -f input_small.txt -w 5

Erwartung & Ergebnis:

- HELLO wird erneut gesendet
- Verbindung wird erfolgreich aufgebaut
- Output korrekt -> PASS

T6

ID: T6
Window: 5
PacketLoss: 0%
AckLoss: 80%
Ziel: Nachweis robuster Verbindungsaufnahme trotz ACK-Verlust.
Server-Command: ./server -p 3340 -f out_T6.txt -r 0.0 -a 0.8 | tee logs/T6_close_ackloss.log
Client-Command: ./client -a ::1 -p 3340 -f input_small.txt -w 5

Erwartung:

- CLOSE-ACK kann verloren gehen
- Client sendet CLOSE erneut
- Server behandelt mehrfachen CLOSE idempotent
- Verbindung wird sauber beendet

Ergebnis:

- diff input_small.txt out_T6.txt → leer
- Mehrfache CLOSE/ACK-Versuche im Log
- PASS

T7

ID: T7
Window: 10
PacketLoss: 10%
AckLoss: 10%
Ziel: Stresstest am oberen Fensterrand (W=10) mit Paket- und ACK-Verlust.
Server-Command: ./server -p 3338 -f out_bonus.txt -r 0.1 -a 0.1 | tee logs/bonus_mixloss.log
Client-Command: ./client -a ::1 -p 3338 -f input_big.txt -w 10

Erwartung:

- Maximal 10 Pakete gleichzeitig im Flug
- Kumulative ACKs funktionieren
- Go-Back-N bei Verlust
- Keine Out-of-Order-Writes