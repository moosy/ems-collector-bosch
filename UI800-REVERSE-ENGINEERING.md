# UI800 (0x90) – Reverse Engineering, Funde (Methode: collectord -d message, Michael ändert am Display)
Der UI800 sendet jede Änderung sofort als 1-Byte-Telegramm an 0x00.
Lesen hinter dem Telegrammende: Leerantwort (type 0x0000, Daten = Typnummer).

## 0x019b (HK1, 10 Byte): 01 03 00 00 00 02 06 28 41 00
- B5 Frostschutz-Art: 0 Aus, 2 Außentemperatur (1/3 vermutl. Raum / Raum+Außen)  ✔
- B6 Frostschutz Grenztemp., int8 °C (Werkswert 5)  ✔
- B8 0x41=65 vermutl. Max. HK1-Temp. (zu prüfen)

## 0x01b9 (HK1, 32 Byte)
- B5 Absenkart: 1 Außentemperaturschwelle, 3 Reduzierter Betrieb (2 vermutl. Raumtemperaturschwelle)  ✔
- B9 Außentemperaturschwelle, int8 °C  ✔
- B12 Durchheizen unter, int8 °C, 0xE1 (−31) = Aus  ✔
- 0x019b B7 Max. HK1-Temp. (bei Michael 40, FBH)  ✔   (B8=65 unbekannt)
## 0x01af (HK1, 14 Byte)
- B2 Raumtemperatur-Offset, int8 K  ✔
- B13 Min. Vorlauftemp.: 0 = nicht verwenden, sonst °C  ✔
## 0xEA am UBA2 (0x88 lesen, UI800 schreibt an 0x08), 24 Byte – WW-Parameter liegen im Kessel!
- B6 WW Komfort ✔ | B11 Einschalthäufigkeit Zirkulation ✔ | B16 Extra-WW ✔ | B18 WW Reduziert ✔ | B20 WW Max. Temperatur ✔
- geraten: B12=70 Desinfektionstemp., B17=60 Grenzwert Max. WW-Temp., B21=100 Max. WW-Leistung %
## Rauschen
- 0x013a (0x90→0x00) zyklisch, 2 Byte Zähler/Wert, nicht von Bedienung
- 0x019b B5 Frostschutz-Art, B6 Grenztemp. (s.o.)
## Urlaub / Abwesend (UI800 sendet an 0x00)
- 0x0065+n: Zeitraum n+1. B0-3 Beginn (Jahr-2000, Monat, Tag, 0), B4-7 Ende (dito)
  B8 Heizen: 2 Ein, 3 Aus (weitere Stufen offen) | B9 Raum-Wunschtemp. °C | B10 Warmwasser: 2 Aus, 4 Reduziert (weitere offen)
  B11 Therm. Desinfektion 0x00/0xFF | B12 "anwenden auf" (1 = HK1?) | Löschen: alles 0, Rest 0xFF
- 0x0a0b+n: Name des Zeitraums, UTF-16BE, 100 Byte (in 25er-Stücken gesendet)
- 0x006d: Byte n = Zeitraum n+1 belegt (0/1)
- 0x006e: Abwesend 0xFF an / 0x00 aus
- Dialog speichert alles auf einmal (Zeitraum-Telegramm komplett + Name).
- "Einstellungen anwenden auf": B13 = Heizkreis einbeziehen (0xFF ja / 0x00 nein), B17 = Warmwasser einbeziehen (0xFF/0x00); B12 = 1 (fest?)
  nur Heizen → B13 FF, B17 00; nur Wasser → B13 00, B17 FF. Dialog sendet nur geänderte Teile (Kopf + geänderte Bytes).
- 0x0065.. lassen sich NICHT per raw read lesen (UI800 antwortet leer) → nur Mitschnitt/Broadcast bzw. evtl. anderer Lesetyp
## WW-System I – weitere Funde
- 0xEA B7 Einschalttemp.-Differenz, int8 K (−2)  ✔
- 0xEA B12 Therm. Desinfektion Temperatur °C (70)  ✔
- 0xEA B17 Tägliche Aufheizung Temperatur °C (60)  ✔   (nicht Grenzwert!)
- 0x01f5 B8 Tägliche Aufheizung aktiv 0xFF/0x00  ✔
- 0x01f5 B9 Tägliche Aufheizung Startzeit in 15-min-Schritten (0x08 = 02:00)  ✔
- 0x01f5 B11 Extra-WW aus beim Aktivieren des UI800-Funktionstests
## Funktionstest UI800 (Diagnose)
- identisch zu unserem: 0x1D[0]=0x5A alle 20 s an 0x00, 0x2D[2]=Pumpe %, [3]=Ventil, [4]=Zirk an 0x08
- WW-Test bietet KEINEN Brenner an (Kessel sperrt Ventil WW + Lüfter, Bosch umgeht das nicht)
- 8 Urlaubsplätze 0x65..0x6c (aus 0xff00-Meldungen)
- 0xEA B5 WW-Bereitung einschalten 0/1 ✔ | B14 Wechselbetrieb mit Heizung 0/1 ✔ | B21 Max. WW-Leistung % ✔
## 0xE6 am UBA2 (Gas-Brennwertgerät > Heizen), per Foto abgeglichen
- B0 Heizbetrieb 0/1 | B1 Max. Vorlauf °C | B4 Max. Heizleistung % | B8 Ausschaltdiff. K | B9 Einschaltdiff. int8 K | B10 Taktsperre min
- vermutl.: B12 Pumpennachlauf min (3), B14 Nachlaufmodulation % (10), B19 Notbetrieb-Vorlauf °C (30)
- 0xE6 B21 Entlüftungsbetrieb: 0 Aus, 1 Auto (Red. Lstg.), 2 Dauerhaft ein  ✔
- 0xE6 B22 Siphonfüllprogramm: 0 Aus, 1 Ein (min. Wärmeerzeugerleistung), 2 Ein (min. Heizleistung)  ✔ (Michael hatte 1; am 09.10. 19:59 auf 0 gestellt)
