# oparl-index

[Projektuebersicht und Build](../README.md) |
[oparl-scan](oparl-scan.md) |
[oparl-search](oparl-search.md)

`oparl-index` baut aus lokalen OParl-Archiven einen gemeinsamen binaeren
Suchindex fuer Metadaten und PDF-Volltexte. Die Originalarchive werden
nicht veraendert. Das Programm benoetigt weder eine Netzwerkverbindung
noch externe PDF-Werkzeuge oder OCR-Unterprozesse.

## Schnellstart

Alle Befehle werden im Wurzelverzeichnis des Repositorys ausgefuehrt.
Unter Linux und Windows den passenden Programmpfad einsetzen.

**Zuerst den Downloadlauf beenden.** Gleichzeitige Aenderungen am
Eingabearchiv ergeben keinen konsistenten Schnappschuss.

```sh
# Gesamten lokalen Bestand ueber alle Jahrgangsarchive indexieren.
build/macos-aarch64/oparl-index --input downloads \
  --output build/search/index.ops

# Alternativ nur ein einzelnes Archiv indexieren.
build/macos-aarch64/oparl-index --input downloads/2024 \
  --output build/search/2024.ops

# Den gemeinsamen Index durchsuchen.
build/macos-aarch64/oparl-search --index build/search/index.ops \
  --year 2024 'Schule OR wohn*'
```

Nach neuen oder aktualisierten Downloads denselben Indexer-Aufruf erneut
ausfuehren. Die aktuelle Version baut den Index **vollstaendig neu**;
eine inkrementelle Indexierung gibt es noch nicht.

## Optionen

| Option | Bedeutung und Standard |
| --- | --- |
| `--input PFAD` | Einzelnes Archiv oder Elternverzeichnis; Standard `downloads` |
| `--output DATEI` | Binaerer Index; Standard `build/search/index.ops` |
| `--help` | Kurze Kommandozeilenhilfe |

Archive werden anhand ihrer `archive.json` gefunden. Auch Verzeichnisse
mit mehr als 1.024 Dateien werden vollstaendig verarbeitet.
Die Jahrgangsauswahl erfolgt ueber die Eingabearchive, nicht ueber
eine eigene `--year`-Option des Indexers.

## Welche Inhalte werden indexiert?

- Archivierte Objekte aus `metadata/`, einschliesslich typisierter
  eingebetteter Objekte.
- Lokale Dokumente aus `documents/`, soweit sie sich als unterstuetzte
  PDFs verarbeiten lassen.
- Namen, Drucksachennummern, Typen, aufgeloeste Organisationen,
  Beschreibungen, vorhandene Beratungsrollen und Ergebnisse.
- Pro PDF-Seite der extrahierte Text mit Quellenpfad und Seitenzahl.
- Beziehungen zwischen Drucksachen und Dateien fuer kontextbezogene
  Suchfilter.

Ungefilterte Suchlaufseiten in `index/`, historische Fassungen in `history/`,
Laufprotokolle in `imports/`/`observations/`, HTTP-Pruefdaten in `cache/`
und `.part`-Dateien sind keine Suchinhalte. Nicht jede beliebige
JSON-Eigenschaft wird zu einem eigenen Suchfeld.

Gleiche OParl-IDs aus verschiedenen Jahrgangsarchiven werden
zusammengefuehrt. Reine eingebettete ID-/Typ-Verweise verdraengen keine
vorhandenen Inhalte. Bei inhaltlichen Antworten entscheidet zuerst der
hashverifizierte Beobachtungszeitpunkt; bei gleichem Abrufstand haben
Archivobjekte Vorrang vor eingebetteten Daten, danach entscheidet `modified`,
ersatzweise die lokale Metadaten-Aenderungszeit. Eine solche Ersatzzeit
begruendet weiterhin keinen RIS-Abrufstand.
Als geloescht markierte Objekte und explizit nichtoeffentliche TOP werden
nicht als Treffer ausgegeben.
Auch die Binaerdateien geloeschter File-Objekte werden nicht als
unzugeordnete Dokumente wieder eingefuehrt.

Nicht zuordenbare lokale Dokumente bleiben unter einer ausdruecklich
lokalen `urn:oparl-search:archive-file:...` auffindbar, nicht unter
erfundenen OParl-URLs. Fuer solche Dateien gibt es keine erfundenen
Originalmetadaten.

`underDirectionOf` bezeichnet eine Zustaendigkeit, nicht verlaesslich
die Urheberschaft. Strukturierte Urheberfelder fehlen im untersuchten
Potsdamer Drucksachenbestand.

## Kanonisches Datenmodell (Format 3)

**Bestehende Indexdateien neu bauen.** Format 1 und 2 werden mit einer ausdruecklichen
Neuaufbau-Meldung abgewiesen; es gibt keinen parallelen Legacy-Lesepfad.
Die Originalarchive bleiben unveraendert. Indexer und Suchprogramm muessen
denselben Formatstand verwenden.

Der Index speichert neben den unveraenderten OParl-Metadaten ein kanonisches
Modell. Es ist ueber `oparl-search --show ID --json`, `/api/object` und das
MCP-Werkzeug `get_object` als Feld `model` lesbar, auch ohne Originalarchiv.

### Identitaeten und Fassungen

- OParl-Objekte, darunter System, Koerperschaft, Paper und Consultation,
  behalten ihre vollstaendige OParl-ID. `context` erhaelt explizite
  Koerperschafts-/Systembeziehungen; `Body.system` darf mit eigenem Beleg
  als Systemkontext eines Objekts abgeleitet werden. `System.body` bezeichnet
  dagegen eine Sammlung, keine einzelne Koerperschaft.
- `metadataOrigin` unterscheidet `archive-resource` (Archivobjekt oder
  Element einer archivierten Sammlung), `embedded` und `archive-file`
  (nur lokal identifizierte Datei). Eingebettete typisierte Objekte erhalten
  eigene Datensaetze. Neuere belegte Inhalte koennen auch aus einer
  eingebetteten Antwort stammen; bei gleichem Stand hat das Archivobjekt Vorrang.
- Dokumentfassungen bekommen eine `urn:oparl-search:document:...` aus
  Ressourcenidentitaet und SHA-256 der tatsaechlich gelesenen Dateibytes.
  `documentVersion` nennt ID, Inhaltshash, Bytezahl sowie den belegten letzten
  Beobachtungs- und Inhaltsabruf in Unix-Sekunden (`observedAtEpochSeconds`,
  `fetchedAtEpochSeconds`, ohne Zeitbeleg `null`). Eine unveraenderte
  File-ID kann bei anderen Bytes daher nicht dieselbe Fassung vortaeuschen.
- Seitenidentitaeten enthalten Fassung und 1-basierte Seitennummer.
  Jeder extrahierte Seitenkoerper hat vorerst genau einen Textabschnitt
  mit stabiler ID, Bytebereich und Extraktionsbeleg. Seine ID enthaelt
  zusaetzlich den Hash des extrahierten Textes. Das sind noch keine
  fachlich segmentierten TOP oder Absatz-Fundstellen.
- Zeitereignisse, etwa ein angegebener Sitzungsstart, besitzen aus Objekt,
  Datumsrolle und Rohwert abgeleitete stabile IDs. Ein Starttermin beweist
  weder Durchfuehrung noch Entscheidung.

Fassungs-, Seiten- und Abschnitts-IDs koennen direkt an `--show` bzw.
`get_object` uebergeben werden. Mit einer Fassungs-ID ist auch `page` moeglich.
Die Antwort behaelt die OParl-ID in `id`; `model.identity` bezeichnet die
konkrete Einheit. Eine alte, im geladenen Snapshot nicht enthaltene Fassung
wird nicht auf die aktuellen Bytes umgebogen, sondern als unbekannt abgewiesen.
Der aktuelle Snapshot enthaelt je File die beim Bau ausgewaehlte Archivfassung.
Die unveraenderlichen Downloadfassungen bleiben im Archiv unter `history/`;
dieser Index ist kein historischer Fassungsbrowser.

Ein Paper ist weder automatisch eine Dokumentrolle (Anfrage, Antwort, Fassung)
noch eine politische Initiative. `documentRole` und `initiative` bleiben ohne
entsprechenden Nachweis `null`. Eltern, Kinder und verwandte Papers sind
gerichtete Kanten: mehrere Eltern und Zyklen bleiben erhalten. Weder der erste
Elternknoten noch ein Kennzeichen-Suffix begruendet eine Initiative.

### Typisierte Beziehungen und Quellen

`model.relations` trennt:

| Gruppe | Bedeutung |
| --- | --- |
| `directSources` | Ausschliesslich explizite `Paper.mainFile` und `Paper.auxiliaryFile` |
| `meetingContext` | Beratungen, TOP, TOP-Beschlussdateien/-Anlagen, Sitzungen, Einladungen und Protokolle |
| `relatedPapers` | Ueber-, untergeordnete und verwandte Papers, jeweils mit eigener Rolle |
| `structure` | Gremium, Zustaendigkeit, Koerperschaft, System und belegter Systemkontext |

Die Kanten behalten Ziel-ID, Rolle, Beleg und gegebenenfalls Beratungsqualitaet
`authoritative`, `preliminary` oder `unknown`. Diese Qualitaet stammt allein
aus `Consultation.authoritative`, nicht aus vermeintlich eindeutigen
Ergebniswoertern. Fehlende Zielobjekte bleiben als ID erhalten.

**Graph-Erreichbarkeit ist keine Dokumentzugehoerigkeit.** Nur die beiden
direkten Paper-Dateibeziehungen erzeugen Suchbesitzer. Der Weg ueber Eltern,
Beratung und Sitzung zu einer Sammelanlage ist kein solcher Beleg. Sitzungspakete
und deren Seiten bleiben eigenstaendig durchsuchbar. Direkt gemeinsam genutzte
Dateien behalten alle belegten Paper-Besitzer. Eine spaetere Zuordnung einzelner
TOP-Abschnitte erfolgt nicht durch ungepruefte transitive Vererbung.

### Datumsrollen und Herkunft

`model.dates` unterscheidet `paper.date`, `document.date`, `meeting.start`,
`meeting.end`, `submittedAt`, `decidedAt`, `publishedAt`, `effectiveAt`, `created`,
`modified`, `observedAt`, `importedAt` und `referenceYear`.

Gespeicherte Rohwerte werden nicht zwischen Rollen verschoben. Insbesondere
sind `Paper.date`, File-`created`, ein PDF-Dokumentdatum und der Einreichungs-
oder Antwortzeitpunkt nicht gleichbedeutend. Sitzungszeiten behalten ihren
Zeitzonenoffset. Referenzjahre werden ausdruecklich aus dem Kennzeichen abgeleitet.
Noch nicht belegte Rollen sind `null`, nicht ein Ersatzdatum oder eine Nullzeit.
PDF-Dokumentdaten und fachliche Dokumentrollen werden in M1 noch nicht erkannt.

Werte und Kanten fuehren `evidence`: Quellobjekt, gegebenenfalls Dokumentfassung,
Seite und Textabschnitt, Quellfeld, Ableitungsregel und `certainty`.
`source-assertion` bedeutet eine Quellenangabe, keine nachgepruefte Tatsache;
`derived` bezeichnet eine Ableitung oder Textextraktion; `exact` eine
technisch exakt erfasste Groesse. Daraus werden keine erfundenen
Konfidenzprozente berechnet. Seitenwarnungen gelten weiterhin.

`importedAt.epochSeconds` ist der eigene Zeitpunkt des lokalen Indeximports
in Unix-Sekunden. Er beweist keinen aktuellen RIS-Abruf. `observedAt` bleibt
ohne protokollierten Abruf unbekannt; Dateisystem-mtime ersetzt diesen
Zeitpunkt nicht. Vorhandene Metadaten-Pruefdaten werden gegen Groesse und
SHA-256 der Rohbytes geprueft. Ein ungueltiger vorhandener Beleg stoppt den
Indexbau mit einer ausdruecklichen Aktualisierungsaufforderung.
`observedAt` beschreibt den verifizierten lokalen Beobachtungsbeleg, bei
eingebetteten Objekten den ihrer umschliessenden Metadatenantwort
(`verified-archive-observation-v1`), nicht ein fachliches Ereignis.
Dokumente werden anhand des sicheren `cache.currentPath` ausgewaehlt und
ebenfalls gegen den Inhaltshash geprueft. Alte Dateinamen oder historische
Fassungen duerfen nicht zufaellig die aktuelle Auswahl ersetzen.
Archive ohne solche Belege bleiben lesbar, aber ohne behaupteten Abrufstand.
Die bisherigen Suchfilter und die `date`-Sortierung werden erst in M3 auf
getrennte Datumsrollen umgestellt; ein gespeicherter Sitzungsstart bedeutet
noch keinen fertigen Sitzungszeitfilter.

### Binaerer Vertrag und Integritaet

Format 3 verwendet einen 232-Byte-Header mit Magic `OPARLIDX`, Version,
Endian-Kennung, 64-Bit-Abschnittsoffsets/-zaehlern, Importzeit und SHA-256.
Alle zehn Abschnitte beginnen an 8-Byte-Grenzen:
Zeichenketten, 88-Byte-Suchrecords, Begriffe, Positionen, direkte Suchbesitzer,
typisierte Kanten, Datumsfakten, Herkunftsbelege, 40-Byte-Dokumentfassungen und
Textabschnitte. Zeichenketten-/Textreferenzen bleiben 32-Bit-Offsets;
die gesamte Datei bleibt kleiner als 4 GiB.

Der SHA-256 ueber Header mit auf null gesetztem Pruefsummenfeld und alle
Abschnitte ist zugleich `snapshotId`. Er bindet Metadaten, Texte, Beziehungen
und Importzeit an genau diesen Indexstand; identische Dokumentfassungen
behalten unabhaengig davon ihre eigenen IDs. Der Leser prueft Abschnitte,
Zeichenketten, Rollen, Quelltypen, Belegverweise, Textbereiche und
Fassungsbeziehungen. Jeder Suchbesitzer muss durch eine typisierte direkte
Paper-Dateikante belegt sein. Auch ein syntaktisch korrekter Graphpfad kann
dadurch nicht als direkte Paper-Quelle eingeschleust werden.

## PDF-Textextraktion und ihre Grenzen

Der Indexer verwendet den PDF-Parser und die Flate-Dekompression von
newos sowie eine eigene seitenbezogene Font-/ToUnicode-Auswertung.
Unterstuetzt werden unter anderem:

- Flate-Streams und indirekte Contents-Arrays.
- Die tatsaechliche Seitenreihenfolge aus dem PDF-Seitenbaum.
- WinAnsi und ausgewaehlte Encoding-Differences.
- ToUnicode-bfchar/bfrange einschliesslich UTF-16-Surrogaten.
- Form-XObjects, Fontzustand, Grafiktransformationen und Textabstaende.
- Direkte und indirekte Breitenarrays einfacher Schriften.

Benachbarte Textlaeufe bleiben auch ueber BT/ET-Grenzen hinweg zusammen;
relative Textpositionen werden vom Textzeilenursprung aus berechnet.
Zeilenendtrennungen vor lateinischen Kleinbuchstaben werden vor der
Indexierung zusammengezogen; Ergaenzungsstriche vor `und`, `oder`,
`bzw` und `sowie` bleiben erhalten. Weiche Trennzeichen werden entfernt.
Das ist eine Heuristik, keine woerterbuchgestuetzte Rechtschreibkorrektur.
Bereits vorhandene falsche Leerzeichen innerhalb eines PDF-Textstrings
werden nicht beliebig geloescht. Fuer diese Textverbesserungen vorhandene
Indizes neu bauen; Original-PDFs und archivierte Metadaten bleiben unveraendert.

Textraeume werden aus Textoperatoren und verfuegbaren Fontbreiten
angenaehert. Das ist kein vollstaendiger PDF-Renderer und keine
garantierte Rekonstruktion von Tabellen, Spalten oder visueller
Lesereihenfolge. Eine **OCR zur Erkennung von Bildtexten findet nicht
statt**. Mischseiten koennen trotz vorhandener Textfragmente weitere
unerkannte Bildtexte enthalten.

## Ausgabedateien und Veroeffentlichung

Bei `--output build/search/index.ops` entstehen:

```text
build/search/index.ops                 # binaerer Suchindex
build/search/index.ops.coverage.jsonl  # lesbarer Abdeckungsbericht
```

Der Index enthaelt Woerterbuch, invertierte Positionslisten mit
Byteoffsets, Metadatensaetze, gespeicherte Texte und Drucksachenbeziehungen.
Er hat eine Formatgrenze unter 4 GiB. Details zum Laden, zur
Integritaetspruefung und zur Suchgeschwindigkeit stehen bei
[oparl-search](oparl-search.md#indexformat-und-suchkern).

Ein fertiger binaerer Index ersetzt seine Vorgaengerversion durch
Umbenennen einer `.part`-Datei. Ein vorheriger Index bleibt bei einem
Abbruch vor dieser Veroeffentlichung erhalten.
Index und Abdeckungsbericht sind separate Dateien, kein gemeinsam
atomar veroeffentlichtes Paar. Der Status im Index selbst ist dessen
verbindlicher Stand.

## Abdeckungsbericht und Vollstaendigkeit

`<index>.coverage.jsonl` protokolliert unvollstaendige Quellarchive,
fehlende oder ungeeignete Dokumente und Seitenwarnungen:

| Bit in `warnings` | Bedeutung |
| --- | --- |
| 1 | Unbekannte oder nicht verlaesslich abbildbare Schriftzeichen |
| 2 | Nicht unterstuetzter oder fehlerhafter Inhaltsstream |
| 4 | Bildseite ohne erkennbaren Nutztext, OCR erforderlich; Seitenstempel zaehlen nicht |
| 8 | Fehlende oder insgesamt nicht verarbeitbare Datei, am File-Datensatz |

Die Bits koennen kombiniert sein. Vorhandener Teiltext bleibt suchbar,
wird aber nicht als vollstaendige Erfassung bezeichnet. PDF-Quellpfad
und Seite bleiben auch bei einer Warnung erhalten.

Archiv- und Extraktionsluecken setzen `incomplete: true`. Das ist auch
in Suchantworten und als CLI-Warnung sichtbar. Ein solcher Index darf
durchsucht werden; keine Treffer bedeuten aber nicht zwingend, dass
ein Sachverhalt im RIS nicht vorkommt.

Im zuvor mit Format 1 vermessenen Bestand 2023 bis 2026 wurden 2.969 Seiten mit
Warnungen erfasst, darunter 792 mit OCR-Bedarf. 22 Dateien waren nicht
als unterstuetzte PDF verarbeitbar, drei referenzierte Dateien fehlten.
Zwei Quellarchive waren bereits als unvollstaendig markiert.
Von 6.772 ausgewaehlten Drucksachen ist eine ausdruecklich als geloescht
markiert und daher kein Suchtreffer; die uebrigen 6.771 sowie weitere
Kontextdrucksachen sind enthalten. Diese Zahlen sind eine
Bestandsaufnahme, kein Vollstaendigkeitsnachweis fuer das gesamte RIS.

Der lokale Format-2-Neuaufbau hat weiterhin 96.136 PDF-Seiten, 2.969
Warnungsseiten und 22 nicht verarbeitbare PDFs. Er macht aber auch
zuvor nur eingebettete Kontextmetadaten zu eigenen Datensaetzen:
insgesamt 160.157 Records und 13.991 File-Objekte ohne archivierte Binaerdatei.
Das sind neu sichtbare Abdeckungsluecken im erweiterten Kontext, keine beim
Neuaufbau geloeschten PDFs. Fehlende Kontextdateien werden nicht automatisch
nachgeladen; Umfang und Aktualitaet der Beschaffung sind Teil von M2.

## Exitcodes

| Code | Bedeutung |
| --- | --- |
| `0` | Index ohne bekannte Archiv- oder Extraktionsluecken veroeffentlicht |
| `1` | Fataler Fehler; kein erfolgreicher Indexneuaufbau |
| `3` | Index veroeffentlicht, aber mit dokumentierten Abdeckungsluecken |

Exitcode `3` ist weder ein vollstaendiger Erfolg noch ein fehlender
Index. Vor einer automatisierten Weiterverarbeitung den
Abdeckungsbericht und den gewuenschten Umgang mit Luecken beachten.

## Implementierung und Tests

Der Importer liegt in [src/oparl-index.c](../src/oparl-index.c),
die PDF-Textextraktion in [src/pdf-text.c](../src/pdf-text.c),
das gemeinsame Indexformat in [src/search.c](../src/search.c), Modellvalidierung
und kanonische Ausgabe in [src/search-model.c](../src/search-model.c).

Die [Such- und Indexertests](../tests/search.mjs) erzeugen temporaere
Archive mit mehr als 1.024 Metadatendateien und synthetischen PDFs.
Sie pruefen unter anderem Schriftabbildungen, Flate-Streams,
Contents-Arrays, Seitenreihenfolge, jahrgangsuebergreifende
Deduplizierung, Loeschmarkierungen, nicht zugeordnete PDFs und
OCR-/Extraktionsluecken. Ein fataler Neuaufbau darf einen vorhandenen
Index nicht ersetzen. Produktivarchive werden dabei nicht beschrieben.
Aufruf und Plattformhinweise stehen unter [Tests](../README.md#tests).

[search-model.mjs](../tests/search-model.mjs) prueft darueber hinaus
mehrere Eltern und Zyklen, unbekannte Initiativen/Datumsrollen,
System-/Koerperschaftskontext, belegte direkte und gemeinsame Anlagen,
stabile bzw. bei Byteaenderung wechselnde Fassungszitate, Zugriff ohne
Originalarchiv und strukturelle Fehler trotz neu berechnetem SHA-256.
Die Suite ist Teil von `make test`.

[archive.mjs](../tests/archive.mjs) prueft unabhaengige Kalender fuer mehrere
Koerperschaften, Zeitfenster ohne historische PDF-/Paper-Rekursion, oeffentliche
TOP, Aktualisierungen und Tombstones. Der lokale HTTP-Anbieter testet zudem
304/HEAD, gleich grosse geaenderte PDF-Bytes, Namensaenderungen, unveraenderliche
Fassungen, belegte Abrufzeiten, Auswahl ueber mehrere Archive, Beschaedigungen
sowie unvollstaendige und begrenzte Importlaeufe.
