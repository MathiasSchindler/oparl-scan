# oparl-search

[Projektuebersicht und Build](../README.md) |
[oparl-scan](oparl-scan.md) |
[oparl-index](oparl-index.md)

`oparl-search` durchsucht den von `oparl-index` gebauten lokalen Index
und liefert Metadaten und extrahierte PDF-Seitentexte mit Quellen.
Die Suche benoetigt weder Zugriff auf das RIS noch externe
Suchdienste.

**Aktueller Stand:** CLI mit Einzelaufruf, Batchbetrieb und Benchmark
sowie ein lokaler Webdienst mit deutscher Suchoberflaeche, JSON-API
und lesendem HTTP-MCP-Endpunkt unter macOS und Linux.

## Schnellstart

Alle Befehle werden im Wurzelverzeichnis des Repositorys ausgefuehrt.
Unter Linux und Windows den passenden Programmpfad einsetzen.
Voraussetzung ist ein mit [oparl-index](oparl-index.md) gebauter Index.

```sh
build/macos-aarch64/oparl-search --year 2024 'Schule OR wohn*'

build/macos-aarch64/oparl-search --index build/search/index.ops \
  --year 2024 --paper-type Antrag 'Schule AND NOT Turnhalle'

# Nur PDF-Seiten, mit maschinenlesbarer Ausgabe.
build/macos-aarch64/oparl-search --year 2024 --kind Page \
  --json '"Landeshauptstadt Potsdam" OR wohn*'

# Nur Drucksachen; Organisation meint Zustaendigkeit, nicht Urheberschaft.
build/macos-aarch64/oparl-search --kind Paper --organization Fraktion \
  'Haushalt OR Verkehr'
```

## Optionen und Betriebsarten

| Option | Bedeutung und Standard |
| --- | --- |
| `--index DATEI` | Suchindex; Standard `build/search/index.ops` |
| `--embeddings VERZEICHNIS` | Optionaler, an den Snapshot gebundener nativer Vektorindex |
| `--model VERZEICHNIS` | Native Modellartefakte; zusammen mit `--embeddings` |
| `--threads N` | Inferenz-Worker, 1 bis 32; Standard 1 |
| `--mode MODUS` | `lexical` (Standard), `semantic` oder `hybrid` |
| `--year YYYY` | Referenzjahr einer Drucksache, 2000 bis 2099, nicht das Datum der PDF |
| `--paper-type TYP` | Exakter Drucksachentyp, ohne Gross-/Kleinschreibung |
| `--organization TEXT` | Teilstring der zustaendigen Organisation, ohne Gross-/Kleinschreibung |
| `--organization-id ID` | Exakte Quellen-ID einer Zustaendigkeit |
| `--actor-id ID` | Exakte ID in expliziten Akteursbeziehungen; keine abgeleitete Urheberschaft |
| `--snapshot-id ID` | Indexstand aus der ersten Antwort; bei `offset > 0` erforderlich |
| `--kind TYP` | Objekttyp, z. B. `Paper`, `File`, `Page`, `Meeting` oder `Organization` |
| `--limit N` | Maximale Zahl je Seite, 1 bis 100; Suche 20, Recherchelisten 10; nicht mit `--show` |
| `--offset N` | Die ersten N sortierten Treffer ueberspringen, 0 bis 4294967295; Standard 0 |
| `--sort REIHENFOLGE` | `relevance` (Standard), `date-desc` oder `date-asc` |
| `--date-role ROLLE` | Datumsrolle fuer Filter und Sortierung; Standard `auto` |
| `--date-from YYYY-MM-DD` | Erster eingeschlossener Quell-Kalendertag |
| `--date-through YYYY-MM-DD` | Letzter eingeschlossener Quell-Kalendertag |
| `--group MODUS` | `record` (CLI/API-Standard), `paper` oder `document` |
| `--json` | Strukturierte Ausgabe statt lesbarer Textausgabe |
| `--batch` | Eine Suchanfrage je stdin-Zeile bei einmal geladenem Index |
| `--benchmark N` | 20 bis 100000 warme Wiederholungen einer Anfrage |
| `--show ID` | Kompakte Metadaten anhand einer OParl-/Fassungs-/Seiten-/Abschnitts-ID abrufen |
| `--view relations`, `--direction incoming\|outgoing\|both` | Mit `--show`: vollstaendige typisierte Beziehungen paginiert statt Metadaten abrufen |
| `--paper ID`, `--meeting ID`, `--agenda ID` | Kompakter Vorgang, Sitzung oder separat paginierte Tagesordnung |
| `--object ID` | Gebuendelte Metadaten; bis zu 20 wiederholte IDs |
| `--options` | Typen, Datumsrollen, Projektionen und ID-Katalog; optional `--kind` |
| `--status` | Importzeit, Snapshot und Abdeckung je Objekttyp |
| `--fields FELDER` | Kommagetrennte skalare Metadatenfelder bei Rechercheabrufen |
| `--raw` | Mit `--show`/`--paper` vollstaendige archivierte Metadaten explizit laden |
| `--page N` | Zusammen mit `--show`: extrahierten PDF-Seitentext abrufen; Seiten ab 1 |
| `--serve` | Lokale Weboberflaeche und JSON-API auf `127.0.0.1` starten |
| `--port N` | Mit `--serve`: TCP-Port, 1 bis 65535; Standard 8080 |
| `--web-root VERZEICHNIS` | Mit `--serve`: lokale PDF-Hovervorschau aus `VERZEICHNIS/pdf/` aktivieren |
| `--publish-pdfs VERZEICHNIS` | Indexierte PDF-Fassungen als gepruefte Kopien nach `VERZEICHNIS/pdf/` publizieren |
| `--input VERZEICHNIS` | Mit `--publish-pdfs`: Archiv oder unmittelbar uebergeordnetes Verzeichnis; Standard `downloads` |
| `--help` | Kurze Kommandozeilenhilfe |

Einzelne Suchanfrage, `--batch`, `--show`, `--serve` und `--publish-pdfs` sind alternative
Betriebsarten. `--benchmark` gehoert zu einer einzelnen Suchanfrage
und kann nicht mit `--batch` oder `--show` kombiniert werden.
Im Webmodus werden Filter pro HTTP-Anfrage gesetzt; CLI-Filter und
`--json` sind dort nicht zulaessig.

## Thematische und hybride Suche

[Der Indexer](oparl-index.md#optionale-native-embeddings) erzeugt den optionalen
Companion. Ohne ihn bleiben Wortsuche und deren Ausgabe unveraendert.

```sh
build/macos-aarch64/oparl-search --index build/search/index.ops \
  --embeddings build/search/embeddings --model build/models/embeddinggemma \
  --threads 12 --mode semantic --group paper \
  'Computersicherheit fuer Lehrerinnen und Lehrer'

build/macos-aarch64/oparl-search --index build/search/index.ops \
  --embeddings build/search/embeddings --model build/models/embeddinggemma \
  --mode hybrid --group paper '21/SVV/1029'

build/macos-aarch64/oparl-search --serve --index build/search/index.ops \
  --embeddings build/search/embeddings --model build/models/embeddinggemma \
  --threads 12 --web-root build/search/web --port 8081
```

CLI, `/api/search?mode=semantic|hybrid`, Webformular und MCP-`search` verwenden
denselben Suchkern. Im Webformular ist **Relevanz - Wortsuche** vorausgewaehlt; Thematisch
und Kombiniert (API-Modus `hybrid`) werden erst bei eingerichtetem Companion
freigeschaltet.
`/api/semantic-status` meldet Titel/Abschnitte und eingeschlossene bzw.
ausgeschlossene Hauptformulare. MCP-`get_index_status` enthaelt dieselbe Abdeckung.

Thematisch behandelt die Anfrage als natuerliche Sprache, nicht als
AND/OR-Ausdruck. Maximal 4096 UTF-8-Bytes und 256 Tokens einschliesslich
Anfragepraefix; zu lange oder leere Anfragen werden abgewiesen, nicht gekuerzt.
Hybrid verwendet weiterhin die Wortsuchsyntax und bettet denselben
Anfragetext zusaetzlich ein.

Thematische Relevanz ist das Skalarprodukt normalisierter 768-dimensionaler
Vektoren (Kosinus). Pro PDF-Seite gilt der beste eingeschlossene Abschnitt,
pro Vorgang bei `group=paper` der beste passende Titel-/Seitenhit.
Hybrid bildet die Vereinigung aller Wort- und Vektorkandidaten und addiert
`BM25 / (1 + BM25)` zur vorhandenen Kosinuskomponente; nur Worttreffer erhalten
allein die normierte Wortkomponente. Das ist eine feste gewichtete Fusion,
kein RRF und keine Wahrscheinlichkeit. Diese erste Gewichtung ist noch keine
empirisch kalibrierte Relevanzmetrik. Thematisch liefert eine Rangfolge des
eingebetteten Bestands, keine kalibrierte Mindestschwelle fuer Relevanz.

Alle Metadaten-/Akteurs-/Datumsfilter, Zaehl- und Gruppierungsregeln sowie die
Snapshot-Pagination gelten fuer den **vollstaendigen Kandidatenbestand**.
Nicht erst Top-25-Listen filtern oder fusionieren. `sort=date-asc|date-desc`
behauptet auch bei semantischen Treffern keine neuen Dokumentdaten.

Die JSON-Felder `semantic.cosine`, `semantic.source` und `semanticText`
kennzeichnen die Zusatzquelle. PDF-Abschnitte behalten SHA-256, echte
PDF-Seiten (`pageFirst`/`pageLast`) und ihre Chunk-ID. `joinedTextStart/End`
sind UTF-8-Bytes im mit zwei Zeilenumbruechen verbundenen Prepare-Seitentext,
**nicht** Bytepositionen in `layout.text` oder Worttrefferpositionen.
Deshalb wird fuer solche Treffer kein erfundener matching-text-TOP-/Tabellenkontext
ausgegeben. Die normale Seitencitation und PDF-Hovervorschau bleiben erhalten;
ein Klick fuehrt weiterhin zum RIS-Original.

Gewichte werden erst bei der ersten thematischen/hybriden Anfrage geladen und
danach im Prozess behalten. Ein angeforderter Modus ohne Companion ergibt
einen expliziten Fehler (HTTP 503); ungueltige Manifeste, Vektoren, PDF-/Text- oder
Snapshot-Zuordnungen verhindern das Laden. Keine stille Wortsuch-Ausweichantwort.

**Speicher:** Zusaetzlich zum rund 2-GiB-Volltextindex sind FP32-Gewichte
(rund 1 GiB), Workspace und auf geeigneten Apple-Silicon-Systemen SME-Panels
einzuplanen. Das bisherige oeffentliche 3-GiB-Dienstlimit reicht dafuer nicht
zuverlaessig; vor einer Bereitstellung Spitzenverbrauch messen und das
Gesamtbudget anpassen. Der oeffentliche Dienst wird durch diese Einrichtung
nicht automatisch veraendert. Linux/x86_64 verwendet den portablen CPU-Pfad,
nicht die Apple-SME-Optimierung; Crossbuilds allein belegen weder Laufzeit noch
vergleichbare Geschwindigkeit.

Gezielte Pruefung mit vorhandenen Modellartefakten:

```sh
make build/macos-aarch64/test-embedding
build/macos-aarch64/test-embedding
node tests/search-embedding.mjs build/macos-aarch64/oparl-prepare \
  build/macos-aarch64/oparl-index build/macos-aarch64/oparl-search \
  build/models/embeddinggemma
```

## Suchsyntax

| Anfrage | Bedeutung |
| --- | --- |
| `Schule Verkehr` | Beide Woerter, implizites AND |
| `Schule AND Verkehr` | Beide Woerter |
| `Schule OR Verkehr` | Mindestens eines der Woerter |
| `Schule AND NOT Turnhalle` | Schule, aber nicht Turnhalle |
| `NOT Turnhalle` | Alle passenden Datensaetze ohne Turnhalle |
| `"sicherer Schulweg"` | Unmittelbar aufeinanderfolgende Woerter |
| `wohn*` | Woerter mit diesem Praefix |
| `(Schule OR Verkehr) AND NOT Turnhalle` | Explizite Gruppierung |

Vorrang ist `NOT`, dann `AND`, dann `OR`. Operatoren werden
gross geschrieben. Die gesamte Anfrage wird als **ein Shellargument**
uebergeben. Das Sternchen ist nur am Wortende erlaubt; eine allgemeine
Wildcardsuche wie `wo*nen` gibt es nicht.

Die Wortsuche ignoriert Gross-/Kleinschreibung, gleicht `ss` und
Eszett sowie zusammengesetzte und zerlegte deutsche Umlaute ab.
Phrasen beziehen sich auf normalisierte Wortpositionen, nicht auf
eine zeichengetreue PDF-Darstellung; sie ueberqueren keine PDF-Seiten
oder Metadatenfeldgrenzen. Satzzeichen ausserhalb von Phrasen trennen
mit AND verknuepfte Woerter.

Eine Anfrage darf hoechstens 16 KiB, 64 normalisierte Suchwoerter
und 32 Verschachtelungsebenen umfassen. Ungueltige Anfragen werden
mit Fehler zurueckgewiesen, nicht als leere Trefferliste ausgegeben.

## Metadatenfilter und Drucksachenkontext

Durchsuchbar sind Namen, Drucksachennummern, Typen, aufgeloeste
Organisationen, Beschreibungen, vorhandene Beratungsrollen und
Ergebnisse sowie die extrahierten PDF-Seitentexte. Das ist keine
Suche ueber jede beliebige JSON-Eigenschaft.

PDF-Seiten werden ueber ihre Beziehungen zu Drucksachen gefiltert.
Alle gesetzten Jahr-/Typ-/Organisationsfilter muessen dabei in
**demselben Drucksachenkontext** zutreffen, nicht in verschiedenen
Drucksachen, die zufaellig dieselbe PDF verwenden. `--kind Page`
schraenkt die Ausgabe auf Seiten ein, ohne diesen Kontext zu verlieren.

`underDirectionOf` bezeichnet eine Zustaendigkeit. Eine verlaessliche
personenbezogene Urhebersuche laesst sich daraus nicht ableiten;
entsprechende strukturierte Urheberfelder fehlen im untersuchten
Potsdamer Drucksachenbestand. `organizationId` sucht exakte
`underDirectionOf`-/`organization`-IDs; `actorId` sucht zusaetzlich
`originatorPerson`, `originatorOrganization` und `person`. Diese
Rohbeziehungen sind keine normalisierte Rollen- oder Namenssuche.
Unbekannte IDs erzeugen Fehler, bekannte IDs ohne passende Datensaetze
eine leere Liste. Der Katalog aus `--options` enthaelt indexierte Objekte;
nur referenzierte, nicht indexierte Akteure bleiben in `actors` sichtbar.

## Treffer und JSON-Ausgabe

Ohne `--json` erscheinen Treffer mit Name beziehungsweise Referenz,
Original-ID, lokalem Quellenpfad, Seite und Textausschnitt.
Die Ausgabe ist standardmaessig nach Relevanz sortiert: Seltene Begriffe
und Vorkommen in Metadaten wiegen staerker; Texthaeufigkeit und
Laengennormalisierung fliessen ein. Gleichstaende werden anhand der
internen Datensatzreihenfolge stabil aufgeloest, nicht zufaellig.
`--limit` begrenzt die angezeigten Treffer, nicht die berechnete
Gesamttrefferzahl. Mit `--offset` sind auch Treffer jenseits der ersten
100 erreichbar.

Mit `--group paper` werden passende Metadaten und PDF-Seiten **vor**
Sortierung, Gesamtzahl und Seiteneinteilung nach Vorgang zusammengefasst.
Eine gemeinsam verwendete Datei erscheint in jedem passenden Paper-Kontext;
alle Metadatenfilter muessen weiterhin denselben Kontext erfuellen.
Die nach der gewaehlten Sortierung beste passende Fundstelle liefert
Ausschnitt, Quelle und PDF-Seite.
Der Gruppenwert ist der beste Einzelwert, keine Summe mit einem Bonus fuer
lange PDFs. `matchCount` zaehlt passende Datensaetze, nicht einzelne
Wortvorkommen. Nicht zuordenbare Dateien werden pro Datei, andere Objekte
weiter einzeln ausgegeben. AND/Phrasen werden weiterhin innerhalb eines
Datensatzes ausgewertet, nicht ueber mehrere Seiten verteilt.

`--group document` zaehlt konkrete archivierte Dokumentfassungen:
File-Metadaten und passende Seiten derselben Fassung bilden eine Gruppe,
auch bei mehreren direkten Paper-Besitzern. File-Objekte ohne im Index
belegte archivierte Fassung werden als `excludedUnversionedRecords` ausgewiesen, nicht als
Dokumentfassungen gezaehlt. Andere Objekttypen gehoeren nicht zu dieser
Grundmenge. `groupIdentity` benennt die Gruppe stabil; `identity` bleibt
die Identitaet der ausgewaehlten Fundstelle.

### Datumsrollen, Zeitbereiche und Auflistungen

`--year` filtert **nur das Referenzjahr eines Papers**, nicht Sitzungsjahr,
Dokumentjahr oder Entscheidungsjahr. Datumsrolle und Datumsbereich sind
davon unabhaengig. `--date-role` steuert Filter und chronologische Sortierung:

- `auto`: Paper und direkt zugeordnete Dateien/Seiten verwenden `paper.date`;
  Meetings, Beratungen und TOP verwenden `meeting.start`; andere Objekte
  verwenden ein belegtes `document.date`.
- Explizite Rollen: `paper.date`, `document.date`, `meeting.start`,
  `meeting.end`, `submittedAt`, `decidedAt`, `publishedAt`, `effectiveAt`,
  `created`, `modified`, `observedAt`, `importedAt`.
- Seiten verwenden Fakten ihrer File-Metadaten. `paper.date` darf nur vom
  passenden direkten Paper-Besitzer stammen. Bei Beratungen/TOP wird eine
  Sitzungszeit nur bei eindeutig belegter typisierter Meeting-Beziehung
  uebernommen; mehrere oder nicht aufloesbare Sitzungen bleiben unbekannt.
- Generische Rollen wie `created` beziehen sich auf das Trefferobjekt bzw.
  den File-Elternsatz einer Seite, nicht automatisch auf das zugehoerige Paper.
  `importedAt` bezeichnet ausschliesslich den lokalen Indeximport.
  `referenceYear` ist keine zulaessige Datumsrolle.

`--date-from` und `--date-through` sind inklusive Grenzen in der **lokalen
Kalenderdatumsangabe der Quelle**, nicht des Browsers oder Servers. Eine
einseitige Grenze ist erlaubt. Gueltig sind gregorianische Tage der Jahre
0001 bis 9999; ungueltige oder umgekehrte Grenzen werden abgelehnt.
Zeiten benoetigen Sekunden und `Z` oder einen Offset wie `+01:00`;
bis zu neun Nachkommastellen bleiben erhalten. Fuer `Meeting.start/end`
reicht ein Tagesdatum ohne belegte Uhrzeit/Zeitzone nicht aus.

`--sort date-desc` und `date-asc` vergleichen echte Zeitpunkte nach UTC,
einschliesslich Offset und Nachkommastellen, auch bei Zeitumstellung.
Reine Tagesdaten behaupten keine Uhrzeit; ihre Sortiergrenze ist 00:00 UTC.
Fehlende und ungueltige Werte stehen ohne Zeitfilter in beiden Richtungen
zuletzt. Gleichstaende werden durch Relevanz und stabile Datensatzreihenfolge
aufgeloest. Bei mehreren passenden Besitzern folgt auch die Auswahl des
ausgegebenen Kontexts dieser Sortierung.

Mit Zeitfilter werden unbekannte/ungueltige Werte nach dem vollstaendigen
Text- und Metadatenabgleich separat als ausgeschlossene **Datensaetze**
gezaehlt (`excludedUnknownDateRecords`, `excludedInvalidDateRecords`).
Diese Zahlen sind keine Paper-Zahlen und keine Negativauskunft zum RIS.
Bei mehreren unklaren Besitzerkontexten koennen beide Gruende denselben
ausgeschlossenen Datensatz betreffen.

Ohne Suchwort ist eine Auflistung mit einschraenkendem Typ-, Referenzjahr-,
Metadaten- oder Zeitbereichsfilter moeglich. Auch `--group document`
definiert eine eigene Grundmenge. Leeres/fehlendes `q` nur mit Sortierung,
Datumsrolle oder Limit ist zu breit und wird abgelehnt. Bekannte Typen ohne
Bestand liefern null Treffer; unbekannte Typen und Rollen liefern Fehler.
`*` ist weiterhin ungueltige Suchsyntax, kein Listing-Workaround.

```sh
# Sitzungen im November ohne kuenstliches Suchwort, nach echtem Beginn.
build/macos-aarch64/oparl-search --kind Meeting --date-role meeting.start \
  --date-from 2026-11-01 --date-through 2026-11-30 --sort date-asc --json
```

```sh
# Zweite Seite mit 50 Treffern, nach Datum absteigend.
build/macos-aarch64/oparl-search --year 2024 --sort date-desc \
  --limit 50 --offset 50 --snapshot-id "$SNAPSHOT_ID" 'Schule OR wohn*'
```

`SNAPSHOT_ID` aus der ersten JSON-Antwort uebernehmen. Fortsetzungen
behalten alle urspruenglichen Filter, Sortierung und Gruppierung.
`next` liefert Offset, Limit und Snapshot; `null` bedeutet das Listenende.
Ein fehlender oder abweichender Snapshot erzeugt einen Fehler, keine
moeglicherweise verschobene zweite Seite. Der Webclient uebernimmt den
Snapshot automatisch; Filteraenderungen starten ohne alten Snapshot neu.

`--json` liefert ein Objekt mit:

- `total`: exakte Zahl passender Einheiten vor Seiteneinteilung.
- `group`: `record`, `paper` oder `document`.
- `countUnit`: `record`, `paper-or-standalone-object` oder `document-version`;
  `matchedRecords` nennt die passende Datensatz-Grundmenge vor Gruppierung.
- `filterOnly`, `dateRole`, `dateFrom`, `dateThrough`, `sort`, `snapshotId`
  und die genannten Ausschlusszaehler machen die Abfrage nachvollziehbar.
- `incomplete`: bekannte Archiv- oder Extraktionsluecken.
- `hits`: die besten Treffer bis zum gewaehlten Limit.

Jeder Treffer enthaelt `id`, `kind`, `name`, `reference`, `date`,
`paperType`, `organization`, `source`, `page`, `warnings`, `matchCount` und `snippet`.
`date` ist jetzt das gewaehlte Filter-/Sortierdatum; `rawDate` erhaelt
die alte, teilweise vom ersten Besitzer abgeleitete Datensatzspalte und
ist **kein Beleg fuer ein Dokumentdatum**. `sortDate` und bei Zeitfilter
`filterDate` enthalten Rohwert, Rolle, Praezision, Quellobjekt, Eigenschaft,
Regel, Belegsicherheit und Uebernahmepfad; unbekannte Fakten bleiben `null`.
Bei Datei-/Seitentreffern enthalten `evidence.documentDates.created` und
`modified` die Zeitstempel des File-Eintrags mit denselben Belegangaben.
Meeting-Quellenkanten nennen mit `sourceDate` den belegten Sitzungsbeginn.
Diese Zusatzangaben ersetzen kein fehlendes Dokumentdatum und aendern
weder Datumsfilter noch Sortierung.
Suchbelege enthalten bei Dokumenten hoechstens vier eingehende Kanten;
andere Treffer tragen ihren Eigenbeleg statt einer kompletten Nachbarschaft.
`evidence.relationshipTotal` zaehlt die passenden Kanten,
`relationshipsComplete` kennzeichnet Vollstaendigkeit.
`relationsAvailable` beschreibt `get_object` mit `view: "relations"`,
Richtung, ID und Snapshot fuer die vollstaendige Nachladung. Bei
Paper-Gruppierung ist der Suchbeleg auf den passenden Besitzer begrenzt;
die Nachladung zeigt alle eingehenden Beziehungen des Objekts.
Fehlende oder gekuerzte Quellenkontexte begruenden keinen eindeutigen
Dokumenttitel oder ein eindeutiges Sitzungsdatum.
Lokale Importzeiten verweisen mit `sourceIndex`/`via: index` auf den Snapshot,
nicht auf eine behauptete OParl-Eigenschaft des Trefferobjekts.
`unit`, `identity` und `groupIdentity` unterscheiden Paper, eigenstaendiges
Objekt, Seite, Beratung und Dokumentfassung. Politische Initiativen,
Wortvorkommen oder zusammengefasste Entscheidungsereignisse werden nicht
aus Paper-/Seitenzahlen erfunden; entsprechende Gruppen sind nicht etabliert.
Datei-/PDF-Seitentreffer enthalten ausserdem `documentUrl`, sofern
die archivierten File-Metadaten eine nicht leere `downloadUrl` oder
ersatzweise `accessUrl` nennen. Die URL wird nicht aus der OParl-ID
oder dem lokalen Dateinamen erraten.
Vorlagentreffer koennen `mainDocument` mit `fileId`, `documentUrl`, `fileName`
und `documentVersion` des eindeutig verknuepften Hauptdokuments enthalten.
Fehlende, nicht aufgeloeste oder mehrdeutige Hauptdokumentbeziehungen werden
nicht durch beliebige Anlagen ersetzt. Der Metadatenbeleg bleibt unveraendert;
die zugehoerige PDF-Fassung ist ein separater Dokumentverweis.
Seitenzahlen beziehen sich auf die PDF-Seitenreihenfolge; Metadaten
haben `page: 0`. Die Warnungsbits sind im
[Abdeckungsbericht](oparl-index.md#abdeckungsbericht-und-vollstaendigkeit)
erklaert.

Bei passenden Dateibeziehungen beschreiben `relatedPaper`, `relatedName`,
`relatedReference`, `relatedDate`, `relatedPaperType` und
`relatedOrganization` einen passenden Drucksachenkontext.
Eine Datei kann mehreren Drucksachen zugeordnet sein; die Antwort
ist keine vollstaendige Liste aller dieser Beziehungen.

Ein lokales, nicht zuordenbares Dokument hat eine
`urn:oparl-search:archive-file:...` statt einer erfundenen OParl-URL.
`evidence.citation` bindet die Fundstelle an `snapshotId`, stabile
Identitaet, Dokumentfassungs-ID, SHA-256 und PDF-Seite.
`evidence.relationshipPath` liefert die expliziten strukturellen
Quellenkanten, etwa `paper.mainFile` oder `meeting.auxiliaryFile`.
Eine fehlende Kante macht ein eigenstaendiges Dokument nicht zum Paper-Inhalt.
Die Antwort liefert keine PDF-Binaerdaten; `source` verweist auf
die lokal archivierte Quelle, `documentUrl` auf das Originaldokument.

## PDF-Befunde in Suchtreffern

CLI-JSON, Websuche und MCP liefern je Treffer `preparation`, soweit ein
vorbereiteter PDF-Beleg existiert, sonst `null`. Die kompakte Vorschau nennt
Formularwerte, Dokumentrolle, Strukturzaehlungen, Feldprobleme und gegebenenfalls
den passenden Protokoll-TOP oder die tatsaechlich getroffene Tabellenzeile.
Ein Paper-Metadatentreffer erhaelt nur die Befunde einer eindeutig belegten
Hauptdatei; Anlagen oder mehrere Hauptdateien werden nicht willkuerlich ersetzt.

`selection: "matching-text"` kennzeichnet einen Kontext an der Textfundstelle.
`"page"` ist lediglich Seitenkontext, `"document"` nur Dokumentkontext.
Bei mehreren TOPs wird ohne passenden Textbeleg keiner herausgegriffen.
Der Kontext ist auf 1.600 serialisierte Bytes begrenzt; ein zu grosser
Kontext bleibt mit `contextOmitted: true` ausdruecklich nachladbar.
Diskussionsvolltexte werden nicht in jeder Trefferkarte wiederholt.

`preparation.available` enthaelt unmittelbar nutzbare Argumente fuer MCP-
`get_object` oder `/api/object`: unveraenderliche Fassungs-ID, Snapshot und
gegebenenfalls PDF-Seite. Der Abruf liefert alle Formularfelder samt
Status, Regel und Quelltext; Seiten ergaenzen `preparation.pageData` mit
vollstaendigen Tabellen, TOPs und vorhandenem `layout.text`.
Die gespeicherten `layout.spans` werden nur mit `raw: true` geliefert;
`layoutSpansAvailable` zeigt ihre Verfuegbarkeit an.
Alle Zell-`sources` sind **UTF-8-Bytebereiche im Layouttext**.
Sie duerfen weder als JavaScript-Zeichenindizes noch als Offsets im
normalisierten Suchtext behandelt werden.

Diese Befunde funktionieren ohne Originalarchiv oder Prepare-Cache.
Leere Zellen, Unsicherheit und Extraktionsfehler bleiben sichtbar.
Ein PDF-Ergebnistext begruendet weder automatisch einen verbindlichen
SVV-Beschluss noch eine neue Graphbeziehung.

## Metadaten und Seitentexte abrufen

```sh
# Die ID aus einem vorherigen Suchtreffer uebernehmen.
build/macos-aarch64/oparl-search --show "$OPARL_ID" --json
build/macos-aarch64/oparl-search --show "$FILE_ID" --page 2 --json
```

Rechercheabrufe liefern immer JSON, auch ohne `--json`. `--show` liefert
standardmaessig kompakte skalare `metadata` samt `citation`, Quellenrolle,
Warnungen und explizitem `omittedFields`/`rawAvailable`. Lange Werte und
eingebettete Arrays/Objekte werden nicht still abgeschnitten.
Vorbereitete Dokumente liefern zusaetzlich `preparation` mit getrennten
PDF-Formularbefunden und ihrer Herkunft. Rohmetadaten bleiben unveraendert;
ein `answer`-Befund beweist keine abschliessende Beantwortung. Siehe
[Vorbereitung, Templates und Grenzen](oparl-prepare.md).
Mit `--raw --show` bleiben die vollstaendigen Rohmetadaten und das
kanonische `model` abrufbar. `--page` bzw. eine Seiten-/Abschnitts-ID liefert
den vollstaendigen extrahierten Seitentext, nicht die visuelle PDF-Seite.
Nicht vorhandene IDs oder Seiten fuehren zu einem Fehler.

```sh
build/macos-aarch64/oparl-search --paper "$PAPER_ID"
build/macos-aarch64/oparl-search --meeting "$MEETING_ID"
build/macos-aarch64/oparl-search --agenda "$MEETING_ID"
build/macos-aarch64/oparl-search --show "$OPARL_ID" --raw
build/macos-aarch64/oparl-search --object "$ID_A" --object "$ID_B" \
  --fields name,reference,paperType,downloadUrl
build/macos-aarch64/oparl-search --options --kind Organization --fields name
```

Feldprojektionen verwenden eine kommagetrennte Zeichenkette, kein Array.
`get_search_options`/`--options` liefert `projectionFields`, `paperTypes`,
`dateRoles`, `kinds` und einen paginierten ID-Katalog.
`--raw` mit Bulk-/TOP-Projektionen erlaubt lange ausgewaehlte skalare
Werte; fuer komplette Rohobjekte `--raw --show` verwenden. Vollstaendige
Einzel-/Vorgangsrohdaten und `--page` lassen sich nicht mit `fields`
kombinieren; Einzelobjekte akzeptieren kein Limit/positives Offset.

## Batchbetrieb

```sh
printf 'Schule\nHaushalt\nwohn*\n' | \
  build/macos-aarch64/oparl-search --batch --json --year 2024
```

Der Prozess laedt und validiert den Index nur einmal und verwendet
seinen Anfragespeicher wieder. Jede Zeile ist eine eigenstaendige
Anfrage; bei `--json` entsteht eine JSON-Antwort je Zeile.
Die per Kommandozeile gesetzten Filter gelten fuer alle Anfragen.
Ein ungueltiger Suchausdruck beendet den Batchlauf mit Fehler.
Ein waehrenddessen neu gebauter Index wird nicht automatisch geladen;
dafuer einen neuen Suchprozess starten.

## Lokaler Webdienst

```sh
build/macos-aarch64/oparl-search --serve \
  --index build/search/index.ops --port 8080
```

Die Suchoberflaeche ist unter <http://127.0.0.1:8080/> erreichbar.
Sie bietet Suchsyntax, Metadatenfilter, Quellen sowie nachladbare
Metadaten und PDF-Seitentexte. Standard sind **50 Treffer pro Seite**;
20 und 100 sind ebenfalls waehlbar. Vor-/Zurueck-Schaltflaechen oberhalb
und unterhalb der Treffer erschliessen den gesamten Ergebnisbestand.
Angezeigt werden Trefferbereich, Gesamttrefferzahl und Seitennummer.
Suchanfrage, Sortierung und Seitengroesse stehen in einem gemeinsamen
Suchbereich oberhalb der Treffer. Ab 1.000 Pixel Ansichtsbreite stehen
Suchanfrage, Sortierung, Seitengroesse und Suchschaltflaeche platzsparend
in einer horizontalen Zeile. Auf Tablets und Handys bricht die Leiste um;
Filter und Hilfe teilen sich eine kompakte Zeile und oeffnen sich bei Bedarf
ueber die volle Breite.
Die Auswahl bietet **Relevanz** mit den
Varianten **Wortsuche**, **Thematisch** und **Kombiniert** sowie **Chronologisch**.
Bei Chronologie kehrt der Pfeil die Richtung um: abwaerts bedeutet
neueste zuerst, aufwaerts aelteste zuerst. Die Ausgangsrichtung ist abwaerts;
spaetere Wechsel behalten die zuletzt gewaehlte Richtung.
Chronologie behaelt die Suchart bei und zeigt sie als Kennzeichnung an.
Eine Datumssortierung kann auch thematisch schwache Treffer nach vorne bringen;
Suchlogik und Treffermenge werden dadurch nicht eingeschraenkt.
Die Bedeutung der Verfahren steht unter "Hilfe zur Suche".
Sortierungs- oder Seitengroessenwechsel beginnen wieder auf Seite 1.

Trefferkarten zeigen Kennzeichen und Titel, eine kompakte Zeile mit Datum,
Typ und Organisation sowie einen einmalig beschrifteten Textauszug.
Die PDF-Seite steht nur am Auszug; Dateiname, technische Datumsrollen und
Erstellungs-/Aenderungszeitstempel wiederholen sich nicht in der Trefferliste.
Unter "Details und Quellen" stehen automatisch erschlossene PDF-Fakten,
passende TOPs mit Ergebnistext, Beratungs-/Tagesordnungszeilen und
die technischen Quellenangaben. Diese Bereiche sind standardmaessig geschlossen;
Extraktionsfehler oder -warnungen bleiben an der Zusammenfassung erkennbar.
Breite Anzeigen nutzen bis zu 1.920 Pixel.
Auf Desktopansichten ab 1.000 Pixel werden Kopfbereich, Trefferabstaende,
Textauszuege und Aktionszeilen kompakter dargestellt; auf kleinen Anzeigen
bleiben die groesseren Abstaende erhalten.
Die Details laden alle Seitenbefunde nach, einschliesslich unsicherer
Formularfelder, Quellenzitaten, lokal scrollbaren Tabellen und Seitennavigation.
Vollstaendige Layoutspannen bleiben der ausdruecklichen JSON-Rohsicht vorbehalten.

PDF-Treffer zeigen den Titel ihrer zugehoerigen Vorlage als Ueberschrift,
sofern vorhanden. Bei generischen Dokumentnamen wie "Sammeldokument
oeffentlich", "Protokoll oeffentlich" oder "Alle Anlagen oeffentlich"
nutzt die Ansicht den Titel eines eindeutig verknuepften TOPs oder einer
Sitzung. Mehrdeutiger oder fehlender Kontext faellt auf den archivierten
Dateinamen zurueck, nicht auf eine erfundene Vorlagenzuordnung.
Die JSON-Suche liefert dafuer `fileName` sowie `sourceName`/`sourceKind`
an den belegten Quellenkanten; Rohname, Gruppierung und Besitzbeziehungen
bleiben unveraendert. Dokumentnamen wie "Sammeldokument oeffentlich" bleiben
in den aufklappbaren Details erhalten. PDF-Seitennummer,
Drucksachenkennzeichen, Typ und Datum bleiben sichtbar.
Alle Treffertitel sind Links: Vorlagen und direkt zugeordnete
Dokumente fuehren zur lokalen Vorgangsseite, Sitzungen zur Sitzungsseite.
Dokumente ohne direkten Vorlagenbesitzer verweisen auf die Original-PDF;
fehlt eine nutzbare Dokument-URL, auf die lokal archivierten Metadaten
beziehungsweise den Seitentext. "Vorgang ansehen" fuehrt zur lokalen Vorgangsseite.
"PDF oeffnen" beziehungsweise "Originaldokument oeffnen" fuehrt
direkt zur archivierten Original-URL, bei Potsdam normalerweise
auf `egov.potsdam.de`, in einem neuen Tab/Fenster. OParl-JSON-Links
stehen in den Metadatendetails, nicht anstelle des Dokumentlinks.
Lange lokale Quellenpfade und Extraktionswarnungen stehen
in den aufklappbaren Details. Die Websuche verwendet standardmaessig
`group=paper`: ein Treffer pro Vorgang mit Hinweis auf weitere passende
Fundstellen. Ausschnitte fassen aufeinanderfolgende Leerraeume zusammen.
Sie erscheinen in einem farblich abgesetzten Zitatfeld, beschriftet als
PDF-Text mit Seite oder als Metadatenauszug. Quellenrollen,
SHA-256 und Fassungslinks stehen nur in den aufklappbaren Details;
die strukturierten Belege in JSON/MCP bleiben erhalten.
Ein fehlendes Dokumentdatum wird ausdruecklich als nicht angegeben
bezeichnet. In den Details stehen ein eindeutig belegter
Sitzungstermin sowie Erstellungs-/Aenderungszeitstempel des Dateieintrags
unter getrennten Beschriftungen. Ein Sitzungs- oder Metadatenzeitstempel
wird nicht als Datum der PDF-Erstellung ausgegeben.
Filter stehen auf allen Bildschirmgroessen oberhalb der Ergebnisse.
"Suche eingrenzen" und "Hilfe zur Suche" sind unabhaengig
von der Bildschirmbreite standardmaessig geschlossen. Bei expliziten
Suchfiltern in der URL wird der Filterbereich geoeffnet.

Suchanfragen, Filter, Sortierung und Seitenposition stehen in der
Browser-URL; Vor-/Zuruecknavigation wird unterstuetzt.
Die Oberflaeche benoetigt JavaScript und laedt keine
externen Skripte oder Schriften. HTML und JavaScript sind im Programm
eingebettet; ein zusaetzliches Webverzeichnis ist nicht erforderlich.

### Vorgangsseite und aufgeloeste Daten

`/paper?id=OPARL_ID` zeigt einen Vorgang als HTML-Oberflaeche:

- Titel, Kennzeichen, Datum, Drucksachentyp, Zustaendigkeit und Beschreibung.
- Hauptdokument und Anlagen mit direkten Original-Links.
- Ueber-, untergeordnete und verwandte Vorlagen, mit Links zu deren
  eigenen lokalen Vorgangsseiten.
- Beratungsfolge mit Gremien, Sitzung, Tagesordnungspunkt und Ergebnis,
  soweit diese Daten im Index vorhanden sind. Beschlussdateien sind an
  ihre konkrete Beratung und ihren TOP gebunden. Einladungen und Protokolle
  sind separat auf der lokalen Sitzungsseite erreichbar.
- Originalvorgang im RIS ueber dessen archiviertes `web`-Feld,
  Metadatendetails und die entsprechende JSON-Ausgabe.

Die HTML-Ansicht benoetigt wie die Suche JavaScript. Dieselben Daten
stehen unabhaengig davon unter `/api/paper?id=OPARL_ID` als JSON bereit:

```sh
curl --get http://127.0.0.1:8080/api/paper \
  --data-urlencode 'id=https://www.potsdam.sitzung-online.de/oparl/papers/2002374'
```

Die Standardantwort hat `id`, `snapshotId`, globale `incomplete`,
`localCoverage`, `total`, `returned`, `offset`, `next` und eine
nach ID deduplizierte Liste `objects`. Das Limit ist eine Obergrenze:
das kompakte Bytebudget kann eine Seite schon vorher beenden.
Der Button "Weitere verknuepfte Objekte laden" erschliesst explizit alle
Fortsetzungen. Jedes Objekt enthaelt `id`, `kind`, `scope`, `indexed`,
`sourceRole` und kompakte `metadata`; indexierte
Objekte zusaetzlich `warnings`, `citation`, `omittedFields`
und `rawAvailable`.
Listen verwenden `relationshipEncoding: "parent-reference-v1"`:
`parentId`, `sourceRole` und `relationshipDirection` beschreiben die
unmittelbare Beziehung zum Elternobjekt, ohne gemeinsame Pfadpraefixe zu
wiederholen. Nach vollstaendiger Pagination lassen sich alle Pfade
rekonstruieren; bei Tagesordnungen steht das Elternobjekt im Feld `meeting`.
`incoming` bedeutet eine Quellkante vom Kind zum Elternobjekt, `outgoing`
eine vom Elternobjekt zum Kind. In `citation` werden `objectId` und
`snapshotId` vom Objekt bzw. Listenumschlag geerbt; eine fehlende `identity`
ist gleich der Objekt-ID. Fassungs-ID, Hash und Seitenzahl bleiben erhalten.
Leere `actors`/`actorTotal` und lokale Archivpfade werden in Listen nicht
wiederholt; Einzel-/Rohabrufe liefern die zusaetzlichen Details.
Die erste Vorgangsseite hat eine `overview` mit Anzahlen direkter Dokumente,
Beratungen und verwandter Papers. Nach Hauptdokumenten folgen Beratungen
nach belegtem Sitzungsbeginn absteigend; unbekannte Zeiten stehen zuletzt.
`scope` unterscheidet Wurzel-Paper (`paper`), direkte Dokumente (`directSource`),
andere Kontextdateien (`contextSource`), verwandte Papers (`relatedPaper`) und
sonstigen Kontext (`context`). Sitzungskontext ist keine direkte Paper-Quelle.
Die ausgewaehlten skalaren Werte bleiben unveraendert; Beziehungen werden
als typisierte Elternverweise geliefert; die Originalrichtung bleibt erhalten.
`indexed: true` bedeutet, dass ein eigener
Datensatz im geladenen Index gefunden wurde, nicht dass die PDF
vollstaendig archiviert oder fehlerfrei als Text erkannt wurde.

Eigene Indexdatensaetze haben Vorrang. Ohne eigenen Datensatz ist im
kompakten Graphen `indexed: false` und `metadata: null`; diese lokalen
Beziehungsluecken stehen unter `localCoverage.unindexedObjects`, getrennt
von globalen Archivluecken. `warningObjects` zaehlt Warnungen an den
verknuepften Datensaetzen, nicht saemtliche Seitenwarnungen einer PDF.
Der explizite Rohabruf `raw=true` bewahrt auch nicht indexierte
eingebettete Quellangaben und geloeschte Markierungen. Es gibt keine
erfundenen PDF-URLs und keine Live-Nachladeanfragen an das RIS.

Beratungsobjekte buendeln `consultation.authority`, Gremium, Sitzungsbeginn,
TOP-Ergebnis und `decisionSources` mit Beschlussdatei/Fassungsbeleg.
Verbindlichkeit stammt ausschliesslich aus dem Quellenfeld `authoritative`:
`true` ist `authoritative`, `false` ist `preliminary`, sonst `unknown`.
Das kennzeichnet die Entscheidungsrolle, beweist allein aber weder ein
vorliegendes Ergebnis noch eine durchgefuehrte Beratung.
Gremien aus einer Sitzung werden mit `meeting.organization` und `via`
belegt, nicht als direkte Beratungsangabe ausgegeben.
Mehr als drei Kontext-/Beschlussbelege oder fuenf Akteursreferenzen werden
mit Gesamtzahl und expliziten Mehr-Indikatoren kenntlich gemacht.
Die betroffenen Quellobjekte sind per `get_object raw:true` nachladbar.

Aufgeloest werden die expliziten Verknuepfungen des angefragten Papers
sowie dessen Beratungen, Tagesordnungspunkte, Sitzungen, Gremien und
zugehoerige Dateien. Untergeordnete oder verwandte Papers werden nicht
rekursiv zu einem unbeschraenkten Gesamtgraphen aufgefaltet; ihre
eigenen Seiten erschliessen die naechste Ebene. Andere Tagesordnungspunkte
derselben Sitzung werden nicht als Teil dieses Vorgangs ausgegeben.
Die rohen Sitzungsmetadaten bleiben dabei unveraendert. Es werden keine
fehlenden Gegenbeziehungen oder Beratungen hinzuerfunden.
Zyklen und mehrfach verwendete Dateien erzeugen keine doppelten Objekte.
Die kompakte Nachbarschaft ist vollstaendig paginierbar. Nur der explizite
vollstaendige Vorgangsrohabruf bleibt auf 4096 Objekte begrenzt und
liefert oberhalb der Grenze einen Fehler. Rohantworten sind auf 16 MiB
begrenzt; grosse Bulk-Projektionen ueber Limit/Felder aufteilen.

`/meeting?id=OPARL_ID` zeigt Sitzung und Dokumente. Die Tagesordnung wird
erst mit "Tagesordnung laden" separat angefordert und explizit fortgesetzt.
`lifecycle` unterscheidet `planned`, `held`, `cancelled`, `unknown`.
`eingeladen`/`geplant` bleiben auch bei vergangener Uhrzeit geplant;
`durchgefuehrt` (auch mit Umlaut) bzw. `abgeschlossen` belegen
Durchfuehrung laut Quelle. `cancelled: true` oder `abgesagt` bezeichnet
Absage. Eine geplante Tagesordnung hat `agendaStatus: provisional`,
andere bleiben ohne zusaetzlichen Beleg `unknown`.
`statusConflict: true` kennzeichnet geplante/abgesagte Sitzungen mit
mindestens einem dokumentierten TOP-Ergebnis. `agendaItemsWithResult`
zaehlt diese TOP, nicht angenommene Beschluesse. Rohstatus und Lifecycle
bleiben unveraendert; der Hinweis beweist fuer sich keine Durchfuehrung.
Die Webansicht zeigt den Quellenwiderspruch an.
Ausdruecklich nichtoeffentliche TOP werden nicht ausgegeben.
`nonPublicReferences`, `unresolvedAgendaReferences` und
`publicStatusUnspecified` trennen Nichtoeffentlichkeit, fehlende Aufloesung
und fehlende Quellflags; `total` umfasst verfuegbare sowie unaufgeloeste
TOP-Referenzen. Sortiert wird nach dem Quellfeld `order`, bei unbekannten
Werten mit stabiler interner Reihenfolge, nicht nach einer erfundenen Nummer.

Die Vorgangsansicht verwendet wie die Suche Indexformat 3.
M4 nutzt vorhandene Format-3-Indizes unveraendert; aeltere Formate
muessen mit dem aktuellen Indexer neu gebaut werden.

### Indexstatus

Die verlinkte Unterseite <http://127.0.0.1:8080/health> zeigt die
Groesse der geladenen Indexdatei in MiB und Bytes, Indexspeicher,
Datensatz-/Begriffszahlen, PDF-Seiten sowie Seiten mit Extraktionswarnungen
und OCR-Bedarf sowie Snapshot, lokale Importzeit und Abdeckung je Typ:
Datensaetze, lokale Warnungen, nicht aufgeloeste Relationsziele und Files
ohne indexierte Dokumentfassung. Fehlende Relationsziele koennen auch
absichtlich nichtoeffentliche/geloeschte Ressourcen betreffen und sind
kein Nachweis fehlender oeffentlicher Inhalte.
Der Indexspeicher umfasst Indexdaten und Beziehungstabellen,
nicht den gesamten Prozess oder wechselnde Anfragepuffer.

Hinweise auf bekannte Archiv-/Texterkennungsluecken stehen hier statt
als prominente Warnung ueber der Suche. `incomplete` bleibt in der API
erhalten. Keine Treffer sind weiterhin kein Nachweis fuer das Fehlen
eines Sachverhalts im RIS. Die Seite meldet den Zustand des **geladenen**
Indexes, nicht den Zustand des entfernten RIS oder einer inzwischen
ersetzten Indexdatei.

Der Prozess laedt und validiert den Index einmal. Er greift danach
weder fuer jede Anfrage auf die Indexdatei zu noch startet er CLI-
Unterprozesse. Anfragen verwenden denselben Suchkern und dieselben
JSON-Ausgabehelfer wie die CLI. Deren Ausgabe nutzt newos' Puffer statt
eines Systemaufrufs pro JSON-Zeichen; Maskierung und Antwortformat
bleiben gleich. Mit Ctrl+C den Dienst beenden.
Ein neuer Index wird **nicht automatisch nachgeladen**; nach einem
Neuaufbau den Dienst neu starten.

### Optionale lokale PDF-Vorschau

Ohne `--web-root` bleiben PDF-Links reine Verweise auf das RIS-Original.
Mit dieser Option zeigt ein mindestens 250 Millisekunden langes
Darueberfahren mit der Maus oder Tastaturfokus eine schwebende Canvas-Vorschau.
Sie zeigt die Trefferseite, bei einem Dateitreffer die erste Seite.
Auch Vorlagentreffer haben einen PDF-Link, wenn ihr Hauptdokument eindeutig
im Index verknuepft ist. Dessen Vorschau zeigt die erste Seite der archivierten
Fassung, nicht eine behauptete PDF-Fundstelle des Metadatentreffers.
Der Klick, das Ziel und das Oeffnen im neuen Tab bleiben unveraendert.
Die Vorschau schliesst nach Verlassen von Link und Vorschau, bei Escape,
Fensterwechsel oder Entfernen des Treffers; sie bleibt innerhalb des Viewports.
Bei schmalen Anzeigen liegt sie oberhalb oder unterhalb statt neben dem Link.
Touch-Klicks werden nicht durch eine Hover-Aktion abgefangen.

```sh
git submodule update --init vendor/pdf-reader
make build/macos-aarch64/oparl-search

build/macos-aarch64/oparl-search --index build/search/index.ops \
  --publish-pdfs build/search/web --input downloads

build/macos-aarch64/oparl-search --serve --index build/search/index.ops \
  --web-root build/search/web --port 8080
```

Der native Publikationslauf liest nur lokale Archive einschliesslich
`history/documents/`; er startet keine RIS-Anfragen, Preparation oder Indexierung.
Er gleicht die tatsaechlichen PDF-Bytes mit den SHA-256-Fassungen des
geladenen Indexes ab, dedupliziert gleiche Inhalte und schreibt unabhaengige,
atomar installierte Kopien nach `pdf/<sha256>.pdf`. Vorhandene Kopien werden
geprueft und wiederverwendet. Fehlende Fassungen oder beschaedigte Kopien
sind ausdrueckliche Fehler (Exit 1); bereits publizierte Kopien bleiben erhalten.
Nach einem Indexneuaufbau den Publikationslauf wiederholen und den Dienst
mit dem neuen Index starten. Veraltete Kopien werden nicht automatisch geloescht.

`/pdf/<sha256>.pdf` unterstuetzt GET/HEAD und liefert nur Fassungen des
geladenen Indexes. Groesse und Hash werden vor jeder Auslieferung geprueft;
Symlinks als PDF-Datei oder unmittelbar konfiguriertes PDF-Verzeichnis
werden abgelehnt. Es gibt keinen beliebigen Dateizugriff, URL-Proxy oder
automatischen externen Fallback. `source` wird nicht zu einem Downloadpfad.

Der CC0-Renderer [pdf-crumb.js](https://github.com/MathiasSchindler/pdf-reader)
ist als Submodul auf Commit `29187bdb1a8079857771342ce2d69f941fd95604` fixiert.
Nur sein vollstaendiges ESM-Bundle mit Diagnostik wird eingebettet, nicht
PDF.js oder die Upstream-Beispieldateien. Renderer und Vorschaucontroller
kommen aus dem C-Binary; der Server benoetigt keine weitere Runtime.
Das Renderer-Modul und die ganze PDF werden erst fuer die Vorschau geladen,
nicht fuer die normale Trefferliste. Das Eingabelimit des Renderers
(128 MiB) wird vor dem PDF-Abruf anhand der indexierten Groesse geprueft;
Laden und Rendern werden auf 15 Sekunden begrenzt. Maximal zwei Dokumente
mit zusammen hoechstens 16 MiB PDF-Eingabedaten bleiben im Vorschaucache.
PDF-Antworten haben zusaetzlich eine Gesamtfrist von zehn Sekunden.

Die Anzeige ist ausdruecklich experimentell: Nicht unterstuetzte Funktionen
und Diagnosewarnungen erscheinen an der Vorschau; Fehler zeigen eine
Fehlermeldung statt einer scheinbar korrekten leeren Seite.
Das RIS-Original bleibt der verlaessliche Ausweg. Es gibt keine Frames,
externen Renderingdienste, OCR oder Ausfuehrung aktiver PDF-Aktionen.
Die CSP erlaubt fuer eingebettete Bilder und Fonts nur notwendige lokale
beziehungsweise Blob-Quellen, keine externen Ressourcen.
Eine oeffentliche Bereitstellung der Kopien ist ein separater Deployment-
und Rechtepruefungsschritt; oeffentliche Abrufbarkeit allein klaert
nicht die Weiterveroeffentlichungsrechte saemtlicher Anlagen.

### JSON-API

Alle Endpunkte sind nur lesend und unterstuetzen `GET` und `HEAD`.
Antworten sind UTF-8-JSON; `HEAD` liefert keinen Antwortkoerper.

| Endpunkt | Parameter | Antwort |
| --- | --- | --- |
| `/api/status` | Keine | Zaehler und Abdeckung, `formatVersion`, `snapshotId`, `importedAtEpochSeconds`, `relations`, `documentVersions`, `textSections` des geladenen Indexes |
| `/api/preview-status` | Keine | `enabled`: ob lokale PDF-Vorschauen mit `--web-root` aktiviert sind |
| `/api/search` | `q` oder einschraenkende Auflistung; `year`, `paperType`, `organization`, `organizationId`, `actorId`, `kind`, `limit`, `offset`, `sort`, `group`, `dateRole`, `dateFrom`, `dateThrough`, `snapshotId` | CLI-Suchstruktur |
| `/api/object` | `id`; optional `page`, `fields`, `raw`, `snapshotId` | Kompaktes Einzelobjekt oder explizite Rohdaten/Seitentext |
| `/api/object?view=relations` | `id`; `direction`, `limit`, `offset`, `snapshotId` | Vollstaendige typisierte Beziehungen, paginiert; nicht mit `page`, `fields` oder `raw` kombinierbar |
| `/api/paper` | Paper-`id`; `fields`, `limit`, `offset`, `snapshotId`, `raw` | Kompakter Vorgang mit Fortsetzung; Rohdaten explizit |
| `/api/meeting` | Meeting-`id`; gleiche Rechercheparameter | Sitzung und Dokumente ohne komplette Tagesordnung |
| `/api/agenda` | Meeting-`id`; gleiche Rechercheparameter | Separat paginierte TOP-Referenzen und Sitzungskontext |
| `/api/objects` | `ids` als URL-kodiertes JSON-Array von 1 bis 20 IDs; Rechercheparameter | Gebuendelte Metadatenprojektion |
| `/api/options` | Optional `kind`, `fields`, `limit`, `offset`, `snapshotId` | Typen, Rollen, Felder und ID-Katalog; Standard Organization |

Werte werden als URL-kodierte Queryparameter uebergeben. `+` wird zu
einem Leerzeichen, Prozentkodierung muss gueltiges UTF-8 ergeben.
Doppelte, unbekannte oder ungueltige Parameter werden abgelehnt,
nicht stillschweigend ignoriert. Grenzen und Bedeutung der Suchfilter
entsprechen der CLI; `paperType` entspricht `--paper-type`.
Ohne `limit` gilt fuer direkte API-Anfragen wie bei der CLI weiterhin
20 fuer Suchanfragen; Recherchelisten verwenden 10 als Obergrenze.
Die Websuche fordert standardmaessig 50 an.
`offset` zaehlt ab 0; auch jenseits des letzten Treffers bleibt
`total` exakt und `hits` ist leer. `sort` akzeptiert `relevance`,
`date-desc` und `date-asc`.
`group` akzeptiert `record`, `paper` und `document`. CLI und direkte JSON-API bleiben
standardmaessig bei einzelnen Datensaetzen; Weboberflaeche und MCP suchen
standardmaessig Vorgangsgruppen.

```sh
curl --get http://127.0.0.1:8080/api/search \
  --data-urlencode 'q=Schule OR wohn*' \
  --data-urlencode 'year=2024' \
  --data-urlencode 'limit=20'

curl --get http://127.0.0.1:8080/api/object \
  --data-urlencode "id=$FILE_ID" \
  --data-urlencode 'page=2'

curl http://127.0.0.1:8080/api/status
```

`curl` ist hier nur ein Beispiel fuer einen API-Client, keine
Laufzeitabhaengigkeit des Dienstes. Metadaten ohne Seite werden durch
Weglassen von `page` abgerufen. PDF-Links behalten ihre archivierte
Original-URL; nur die optionale Hovervorschau nutzt mit `--web-root`
unveraenderliche lokale PDF-Kopien. `source` bleibt ein lokaler Quellenpfad,
kein Download-Endpunkt.

Fehlerantworten haben die Form `{"error":"Beschreibung"}`:

| HTTP-Status | Bedeutung |
| --- | --- |
| `400` | Ungueltige Anfrage, Parameter oder Suchsyntax |
| `403` | Nicht erlaubter Host oder herkunftsfremde API-Anfrage |
| `404` | Unbekannter Endpunkt oder nicht vorhandene ID/Seite |
| `405` | Nicht erlaubte Methode; nur `/mcp` erlaubt POST |
| `408` | Zeitlimit fuer die vollstaendige Anfrage ueberschritten |
| `409` | Snapshot stimmt nicht mit dem geladenen Index ueberein; Recherche neu beginnen |
| `431` | Anfrageheader einschliesslich URL groesser als 64 KiB |
| `500` | Interner Fehler, etwa fehlender Anfragespeicher |
| `503` | Alle Verbindungsplaetze belegt |

Fehler erscheinen zusaetzlich auf stderr. Abgebrochene Clients
beenden nicht den Suchdienst. Ein unvollstaendiger Index bleibt
durchsuchbar und wird weiterhin mit `incomplete: true` gekennzeichnet.

### HTTP-MCP

Der bestehende Dienst stellt unter `/mcp` einen **Streamable-HTTP**-Transport
bereit, lokal beispielsweise `http://127.0.0.1:8080/mcp`.
Die Praxisnachbesserungen von M1-M4 werden zunaechst lokal geprueft;
eine Bereitstellung auf dem oeffentlichen Server ist ein eigener Schritt.
Es gibt keinen zusaetzlichen Port oder Prozess und keine MCP-SDK-
Laufzeitabhaengigkeit.

| Werkzeug | Argumente | Ergebnis |
| --- | --- | --- |
| `search` | Optional `q` bei einschraenkender Auflistung; dieselben Filter wie `/api/search`, einschliesslich `group` und Datumsrollen/-grenzen | Suchantwort; Standard 20 Treffer und `group=paper` |
| `get_object` | `id`; `page`, `fields`, `raw`, `snapshotId`; alternativ `view: "relations"` mit `direction`, `limit`, `offset` | Kompakte Metadaten; Rohmodell/Seitentext oder vollstaendige Beziehungen explizit |
| `get_paper` | Paper-`id`; Rechercheparameter | Kompakter Vorgang wie `/api/paper` |
| `get_meeting` | Meeting-`id`; Rechercheparameter | Sitzung und Dokumente |
| `get_agenda` | Meeting-`id`; Rechercheparameter | Separate paginierte TOP |
| `get_objects` | `ids` mit 1 bis 20 IDs; Rechercheparameter | Gebuendelte skalare Projektionen |
| `get_search_options` | Optional `kind`; Rechercheparameter ohne `raw` | Rollen, Typen, Felder und ID-Katalog |
| `get_index_status` | Keine | Indexgroesse, Zaehler und Abdeckung wie `/api/status` |

Alle Werkzeuge sind nur lesend und greifen ausschliesslich auf den geladenen
Index zu, nicht auf beliebige Dateien oder URLs. Erfolgreiche Aufrufe liefern
die Daten in `structuredContent`. Das Protokoll `2026-07-28` erhaelt
nur einen kurzen Texthinweis statt einer zweiten JSON-Kopie. Aeltere
Protokolle behalten zusaetzlich vollstaendiges JSON im Textinhalt,
damit textbasierte Clients weiterhin funktionieren.
Unbekannte Werkzeuge und fehlerhafte RPC-Nachrichten erzeugen JSON-RPC-Fehler;
ungueltige Werkzeugargumente, Suchsyntax und fehlende Objekte liefern
`isError: true` mit Beschreibung statt erfundener Ergebnisse.

Rechercheparameter sind `fields` (CSV), `limit`, `offset`, `snapshotId`
und gegebenenfalls `raw`. Standard-Vorgangsantworten halten ein kompaktes
Bytebudget ein, einschliesslich Text-/Strukturkopie und JSON-RPC-Huelle
in den Abnahmetests unter 20.000 HTTP-Body-Bytes.
Rohdaten, explizit grosse Suchlisten und komplette Seitentexte sind keine
kompakten Standard-Vorgangsantworten.
Bei `next != null` die urspruenglichen Argumente beibehalten und
Offset/Limit/Snapshot aus `next` uebernehmen. Insbesondere Bulk-IDs
bleiben beim Client; `retainArguments: ["ids"]` macht das ausdruecklich.
`operation` und `retainArguments` sind Fortsetzungsmetadaten, keine
Werkzeugargumente. Beispielsweise in JavaScript:

```js
const { operation, retainArguments, ...cursor } = result.next;
const argumentsForNextCall = { ...originalArguments, ...cursor };
// Werkzeug operation mit argumentsForNextCall aufrufen.
```

`get_object` mit `view: "relations"` liefert `relationships`, `total`,
`returned` und `next`; `direction` ist `incoming`, `outgoing` oder
standardmaessig `both`. Selbstkanten erscheinen nur einmal. Ausdruecklich
nichtoeffentliche TOP-Referenzen bleiben ausgeschlossen und werden
separat gezaehlt. Kanten enthalten Original-IDs, Rolle, Autoritaet,
Quellfeld, Ableitungsregel und Belegsicherheit. Lange Quellnamen werden
mit `sourceNameOmitted` zur expliziten Einzelabfrage zurueckgestellt.

Abdeckung bleibt beweisbezogen: `coverage.perType[].fileVersionReasons`
trennt beim Indexaufbau nicht archivierte Dateien, unbekannte Ursachen
und vorhandene Binaerdaten mit fehlgeschlagener Extraktion.
`unresolvedReasons` trennt ausdruecklich nichtoeffentliche Referenzen
von unbekannten Ursachen. `documentCoverage` beschreibt dies am File.
Diese Angaben belegen keine bestimmte HTTP-Fehlerursache und keinen
fehlgeschlagenen Download; fehlende Ursachen werden nicht erraten.

Kein Client darf eine erste Teilseite als vollstaendiges Negativergebnis
interpretieren. Zitieren ueber Fassungs-ID, SHA-256, Seite und Snapshot;
Original-URLs koennen spaeter andere Bytes liefern.

Unterstuetzt werden:

- **2026-07-28:** zustandslose Anfragen mit `server/discover`, `resultType`
  und Pflichtfeldern fuer Protokollversion und Client-Capabilities in
  `params._meta`. `MCP-Protocol-Version` und `Mcp-Method` muessen dem Body
  entsprechen; bei `tools/call` auch `Mcp-Name` (einschliesslich Base64-Sentinel).
- **2025-11-25, 2025-06-18 und 2025-03-26:** `initialize`, danach
  `notifications/initialized` und der ausgehandelte `MCP-Protocol-Version`-
  Header. Der Server vergibt keine Session-ID.

Zum Beispiel mit einem Client fuer die Version 2025-11-25:

```json
{
  "servers": {
    "potsdam-ris": {
      "type": "http",
      "url": "http://178.254.23.157:8080/mcp"
    }
  }
}
```

Dies ist das VS-Code-Format fuer `.vscode/mcp.json`; andere Clients verwenden
ihre eigene Konfigurationsstruktur. Entscheidend sind HTTP-Transport und URL.
Oeffentliches HTTP ohne TLS wird nicht von jedem Client akzeptiert.

Einzelner Werkzeugaufruf nach der Initialisierung:

```sh
curl http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  --data '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"search","arguments":{"q":"Noosha","sort":"date-desc"}}}'
```

Jeder POST enthaelt genau eine JSON-RPC-Nachricht, mit `Content-Length`
und hoechstens 64 KiB Body. JSON-RPC-Batches, Chunked-Bodys und `Expect`
werden abgelehnt. Akzeptierte Notifications erhalten HTTP 202 ohne Body,
Anfragen eine einzelne JSON-Antwort; GET/DELETE auf `/mcp` erhalten 405.
Es gibt keine SSE-Dauerverbindungen, Subscriptions oder serverinitiierten
Anfragen. Serialisierte Werkzeugdaten sind auf 16 MiB begrenzt; groessere
Ergebnisse verursachen einen ausdruecklichen Fehler statt stiller Kuerzung.
Host-/Origin-Pruefung und die uebrigen Dienstgrenzen gelten auch fuer MCP.

Protokollgrundlagen:
[2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
und [2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).

### Betriebsgrenzen

- Feste Bindung an `127.0.0.1`, kein oeffentlicher oder LAN-Listener.
- Keine Anmeldung und kein TLS im Suchprogramm. Ein oeffentlicher
  Listener gehoert in einen separat abgesicherten Reverse Proxy,
  nicht in das Suchprogramm; ein konkretes Setup folgt unten.
- Host-Header muessen `127.0.0.1` oder `localhost` mit dem passenden
  Port nennen. API und MCP weisen fremde Origins und herkunftsfremde
  Browseranfragen zurueck; es gibt keine allgemeine CORS-Freigabe.
- Hoechstens acht angenommene Verbindungen. Unvollstaendige Anfragen
  haben insgesamt fuenf Sekunden Zeit; blockierte Schreiboperationen
  haben ebenfalls ein Fuenf-Sekunden-Zeitlimit.
- Header werden ueber Polling unabhaengig gesammelt; eigentliche
  Suchanfragen und Antworten werden nacheinander bearbeitet.
  Es gibt noch keinen parallelen Anfrage-Workerpool.
- Request-Bodys ausschliesslich fuer POST `/mcp`, kein HTTP-Keep-Alive oder Pipelining.
  Jede Antwort beendet ihre Verbindung.

Auf Windows meldet `--serve` einen ausdruecklichen Fehler, weil das
eingebundene newos noch keinen TCP-Listener fuer Windows implementiert.
CLI, Batchbetrieb und Indexdateien bleiben davon unberuehrt.

### Bereitstellung auf dem virtuellen Server

Das Setup unter [deploy/](../deploy/) betreibt die Suche auf
<http://178.254.23.157:8080/>. Der Suchprozess selbst bleibt auf
`127.0.0.1:18080`. Eine **eigene Apache-Instanz** bindet ausschliesslich
an die oeffentliche IP mit Port 8080. Die vorhandene Apache-Instanz
auf Port 80 und deren Backend auf `127.0.0.1:8080` bleiben unveraendert.
Die neue Instanz laedt keine Konfiguration der bestehenden Websites.

Voraussetzungen sind Linux/x86_64, systemd und Apache 2.4 mit den
in [apache.conf.example](../deploy/apache.conf.example) geladenen Modulen. Das Setup
verwendet den bereits installierten Apache als Proxy; das Suchprogramm
selbst bleibt freestanding und ohne externe Laufzeitbibliotheken.
Bei einem anderen Server IP-Adresse, Ports und Modulpfade anpassen.

Fuer eine neue Installation die Vorlage `deploy/apache.conf.example`
nach `deploy/apache.conf` kopieren. Eine bereits vorhandene lokale
Konfiguration dabei nicht ueberschreiben. In der neuen Datei
`PublicAddress` von der Dokumentationsadresse `192.0.2.1` auf die echte
Server-IP aendern und bei Bedarf Ports und Modulpfade anpassen.
Nur die konkrete `deploy/apache.conf` wird von Git ignoriert;
die Vorlage und beide systemd-Dienstdateien bleiben versionierbar.

Auf dem Server werden folgende Dateien installiert:

| Datei | Zweck |
| --- | --- |
| `/opt/oparl-search/oparl-search` | Linux-Binary aus `build/linux-x86_64/` |
| `/opt/oparl-search/index.ops` | Suchindex aus `build/search/` |
| `/opt/oparl-search/index.ops.coverage.jsonl` | Abdeckungsbericht zum Index |
| `/opt/oparl-search/web/pdf/<sha256>.pdf` | Gepruefte unveraenderliche PDF-Kopien fuer die Hovervorschau |
| `/etc/oparl-search/apache.conf` | Eigenstaendige Proxykonfiguration aus der lokalen `deploy/apache.conf` |
| `/etc/systemd/system/oparl-search.service` | Suchprozess als Benutzer `oparl-search` |
| `/etc/systemd/system/oparl-search-proxy.service` | Proxy als Benutzer `oparl-search-proxy` |

Die beiden Systemkonten haben keine Anmeldung und kein Homeverzeichnis.
Programme, Index und Konfiguration gehoeren root und sind fuer die
Dienstkonten nicht beschreibbar. Die Units begrenzen Speicher, CPU,
Dateideskriptoren und Prozesse; sie verwenden schreibgeschuetzte
Systemverzeichnisse und keine zusaetzlichen Capabilities.

Der Proxy prueft den oeffentlichen Host und vorhandene Origin-Header,
bevor er Host und erlaubte Origins fuer das lokale Backend umsetzt.
Er aktiviert kein CORS und keinen Forward Proxy. `Sec-Fetch-Site`
wird nicht entfernt; die Herkunftspruefung der API bleibt aktiv.
POST und Request-Bodys sind ausschliesslich fuer `/mcp` freigegeben,
mit 64 KiB Bodylimit und weiterhin ohne Chunked/Expect.
Header-Lesezeiten, Backendverbindungen und Worker sind begrenzt.
Ein zusaetzliches Access-Log mit Suchanfragen wird nicht geschrieben.

**Dies ist ein oeffentlich lesbarer HTTP-Dienst ohne Anmeldung oder TLS.**
Nicht fuer vertrauliche Anfragen oder beliebige hohe Last einsetzen.
Fuer HTTPS zuerst einen passenden Domainnamen und TLS am Proxy einrichten.
Der Klick oeffnet PDF-Dateien weiterhin beim Originalanbieter.
Die Dienstvorlage aktiviert mit `--web-root /opt/oparl-search/web` die
lokale Hovervorschau. Dazu die mit `--publish-pdfs` erzeugten PDF-Kopien
einschliesslich ihrer SHA-256-Pruefsummen uebertragen und vor der Aktivierung
pruefen; das Verzeichnis und die Dateien gehoeren root und sind fuer das
Dienstkonto nur lesbar. Ohne Vorschau genuegt der selbststaendige Index
mit Metadaten und Volltexten; dann `--web-root` aus der Unit entfernen.
Webroot und `pdf/` muessen echte Verzeichnisse sein, keine Symlinks.
Die Originalarchive und der Preparation-Cache muessen nicht uebertragen werden.
`source` bleibt dabei ein Herkunftspfad aus dem urspruenglichen Archiv.

Nach Installation und Konfigurationspruefung auf dem Server:

```sh
systemd-analyze verify /etc/systemd/system/oparl-search{,-proxy}.service
systemctl daemon-reload
systemctl enable --now oparl-search.service oparl-search-proxy.service
systemctl status oparl-search.service oparl-search-proxy.service --no-pager
```

Diese Befehle starten beziehungsweise pruefen **nur die neuen Units**;
`apache2.service` muss nicht neu geladen oder neu gestartet werden.
Backendmeldungen stehen in `journalctl -u oparl-search.service`;
Proxyfehler in `/var/log/oparl-search-proxy/error.log`.
Die Suche zeigt unter `/health` weiterhin die bekannten Indexluecken.

Nach Paketupdates von Apache oder seinen Modulen auch die eigene
Proxy-Instanz mit `systemctl restart oparl-search-proxy.service`
neu starten. Ein Neustart der Standardinstanz `apache2.service` allein
aktualisiert den separaten Proxyprozess nicht. Vorher beide
Apache-Konfigurationen pruefen; dabei sind kurze Unterbrechungen moeglich.

Die externe Pruefung vergleicht Suchantworten bytegleich mit der lokalen
CLI und testet Index-Snapshot, Vorgangsseiten, JSON, HEAD, die acht
MCP-Werkzeuge, Snapshot-Pagination sowie die Host-/Origin-Grenzen.
Sie prueft auch die aktivierte PDF-Vorschau, Renderer-Bytes und PDF-GET/HEAD:

```sh
node tests/search-deployment.mjs http://178.254.23.157:8080/ \
  build/macos-aarch64/oparl-search build/search/index.ops
```

Diese optionale Pruefung verwendet den realen Potsdamer Index fuer
2021 bis 2026 und die lokalen publizierten Kopien unter `web/pdf/` neben
dem Index; sie laeuft nicht als Teil der kleinen Fixture-Tests.

Fuer eine Aktualisierung den neuen Index zunaechst unter einem anderen
Dateinamen uebertragen, SHA-256 mit dem lokalen Original vergleichen,
dann auf demselben Dateisystem atomar nach `index.ops` umbenennen und
`systemctl restart oparl-search.service` ausfuehren. Den Abdeckungsbericht
ebenfalls aktualisieren. Den Suchindex weiterhin lokal mit `oparl-index`
bauen; der Server laedt nicht automatisch neue RIS-Daten nach.
Bei Programm- oder Indexformatwechseln Binary und Index gemeinsam
aktualisieren und die passende bisherige Kombination fuer eine
Ruecknahme sichern. Den bereitgestellten neuen Stand vor dem Umschalten
mit `--status --json` pruefen.
Bei aktivierter Vorschau ausserdem alle im neuen Index benoetigten PDF-Fassungen
vor dem Neustart publizieren und ihre uebertragenen SHA-256-Pruefsummen pruefen.
Neue Daten zunaechst in einem separaten Release-Verzeichnis bereitstellen;
die bisherige Kombination aus Binary, Index, Unit und realem Webroot
bis nach der oeffentlichen Abnahme fuer eine Ruecknahme behalten.

## Vollstaendigkeit und Exitcodes

- `0`: Anfrage beziehungsweise Inhaltsabruf erfolgreich; auch bei null Treffern.
- `1`: Ungueltige Optionen oder Anfrage, fehlende ID/Seite,
  beschaedigter Index, Speicher-, Lese- oder Ausgabefehler.

Ein als unvollstaendig markierter Index darf durchsucht werden.
Die Suche liefert dann bei Erfolg trotzdem Exitcode `0`, gibt aber
eine Warnung auf stderr und `incomplete: true` in JSON-Antworten aus.
Keine Treffer bedeuten deshalb nicht zwingend, dass ein Sachverhalt
im RIS nicht vorkommt. Die [PDF-/OCR-Grenzen](oparl-index.md#pdf-textextraktion-und-ihre-grenzen)
und die Qualitaet der Quellarchive bleiben relevant.

## Indexformat und Suchkern

Die Suche liest **Format 3, 4 und 5**. Format 4 ergaenzt die getrennten
Dokumentbefunde von `oparl-prepare`; Format 5 bewahrt auch vollstaendige
Seitenstrukturen und vorhandene Layoutdaten. Aeltere unterstuetzte Indizes
bleiben lesbar, enthalten aber keine nachladbaren Seitenstrukturen.
Format 1 und 2 erfordern weiterhin einen Neuaufbau; die Fehlermeldung
nennt dafuer `oparl-index`.
Das [kanonische Datenmodell](oparl-index.md#kanonisches-datenmodell-format-3)
beschreibt typisierte Kanten, Datumsrollen, Fassungsidentitaeten und Belege.
Nur explizite Hauptdateien und Anlagen eines Papers gelten fuer Suchfilter
und Gruppierung als dessen Dokumente. Erreichbare Eltern, TOP und
Sitzungspakete sind dagegen Kontext, keine vererbten Paper-Inhalte.
M3 benoetigt keine neue Diskformatversion: Format 3 enthaelt bereits die
belegten Rohfakten. Beim validierten Laden werden UTC-/Tages-Schluessel,
Typ-Postings, direkte Besitzer-, eindeutige Sitzungskontext- und
Dokumentfassungs-Lookups einmalig vorab aufgebaut. Suchanfragen parsen
keine archivierten Datumstexte oder JSON-Metadaten erneut.

`--show ID --json`, `/api/object` und MCP-`get_object` liefern dieses Modell
gemeinsam mit Rohmetadaten und Text als `model`. Unbekannte Werte bleiben
`null`; ein Paper ist nicht automatisch eine Initiative oder Sachantwort.
Neben OParl-IDs werden konkrete Dokumentfassungs-, Seiten- und Abschnitts-IDs
akzeptiert. Beispiel: die ID aus `model.documentVersion.id` statt einer
moeglicherweise spaeter geaenderten File-ID zitieren, dazu Seite und
`model.snapshotId`. Nur Fassungen im geladenen Snapshot sind abrufbar.
Die Importzeit bezeichnet den lokalen Indeximport, keine neue RIS-Abfrage.
`model.dates.observedAt` und die Abrufzeiten in `model.documentVersion`
kommen dagegen aus hashverifizierten Archivbelegen. Ohne solchen Zeitbeleg
bleiben sie `null`; eine Revalidierung aendert nicht den letzten Inhaltsabruf.
Alle Angaben werden aus dem geladenen Index gelesen, nicht aus Quellpfaden
nachgeladen. Das gilt auch, wenn das Originalarchiv nicht vorhanden ist.

Der unveraenderliche Index enthaelt ein lexikalisch sortiertes
Woerterbuch, invertierte Positionslisten mit Byteoffsets, Metadatensaetze,
gespeicherte Texte und Drucksachenbeziehungen. Exakte Woerter und
Praefixbereiche werden binaer gefunden. Positionslisten ermoeglichen
Phrasenpruefungen ohne erneutes Lesen der PDFs. Die Relevanzsortierung
verwendet IDF, Laengennormalisierung und BM25-artige Gewichte, mit
hoeherem Gewicht fuer Metadaten.

`oparl-search` liest den Index einmal in den RAM und prueft
Format, Abschnittsgrenzen und SHA-256 einschliesslich des Headers.
Die Pruefsumme wird waehrend des Einlesens berechnet; Zeichenketten
werden wortweise und ohne Zugriffe ausserhalb ihres Abschnitts validiert.
Auf macOS/ARM64 nutzt der Build die vorhandene ARM-SHA-256-Implementierung
von newos mit den auf Apple Silicon garantierten SHA-Instruktionen.
Die vollstaendige Integritaetspruefung wird beschleunigt, nicht
ausgelassen oder durch einen ungeprueften Cache ersetzt.
Generische Linux-/Windows-ARM64-Builds behalten die portable Implementierung.

Die [systemd-Vorlage](../deploy/oparl-search.service) erlaubt dem Suchbackend
3 GiB Speicher. Das laesst beim Index der Jahrgaenge 2021 bis 2026 von rund 2,1 GB
Platz fuer Validierung, Zuordnungstabellen und laufende Anfragen.

Beim Laden entstehen kompakte Zuordnungstabellen fuer die einmal pro
Datei gespeicherten Drucksachenbeziehungen. Metadatenfilter werden pro
Anfrage nur fuer benoetigte Kandidaten und deren zugehoerige Drucksachen
ausgewertet. Boolean-Ausdruecke werden auf 64-Bit-Treffermengen
ausgewertet; die Rangfolge wird nur fuer passende Kandidaten bestimmt.
Auch reine NOT-Anfragen und exakte Gesamttrefferzahlen bleiben unterstuetzt.
Die Top-k-Auswahl verwendet einen Heap mit hoechstens `offset + limit`
Eintraegen, begrenzt durch die Datensatzzahl. Es wird nicht erst eine
kurze Trefferliste sortiert: Datumsreihenfolge und Seitenauswahl gelten
fuer die gesamte Treffermenge. Sehr tiefe Seiten benoetigen mehr
Arbeitsspeicher und Rangordnungsarbeit als die erste Seite.
Der Anfragespeicher wird bei `--batch` und `--benchmark` wiederverwendet,
nicht die Ergebnisse vorheriger Anfragen.

Der Suchkern in [src/search.c](../src/search.c) wird direkt vom
[CLI](../src/oparl-search.c) verwendet. Jeder parallele Aufrufer
benoetigt einen eigenen Arbeitsspeicher; der Index bleibt unveraenderlich.
Die Optimierungen erfordern keinen Neuaufbau bestehender Indizes.
Der Index hat eine Formatgrenze unter 4 GiB; zu grosse Kombinationen
aus Index, Zuordnungstabellen und Anfrage-Arbeitsspeicher werden mit
Fehler zurueckgewiesen.

Es gibt noch kein mmap, komprimierte Postinglisten, SIMD oder WAND:
Solche Optimierungen sollen durch Messungen begruendet werden, nicht
durch ein unbelegbares Versprechen der absolut schnellsten Suche.

## Benchmarks

Die vereinbarte Messgroesse ist warme Top-20-Wortsuche mit Metadatenfiltern
bei p95 unter 10 ms und maximal 4 GiB Such-RAM auf dem aktuellen Mac.

```sh
build/macos-aarch64/oparl-search --year 2024 --paper-type Antrag \
  --benchmark 300 Schule
make benchmark
```

Die Benchmarkausgabe trennt Laden und Validieren des Indexes,
erste Anfrage und warme p50/p95/p99. Anfrageparsing, Filter,
Retrieval und Top-k-Auswahl sind eingeschlossen; Ergebnisausgabe und
Transport nicht. Phrase und Praefix werden separat gemessen.
Ein neuer Prozess ist **kein Nachweis eines physisch kalten Datentraegers**:
Der Betriebssystem-Dateicache wird nicht geleert.
Der optionale [Node-Testtreiber](../tests/benchmark-search.mjs) verwendet
auf macOS `/usr/bin/time -l` fuer den maximalen residenten Speicher.
Die eigentlichen Programme starten keine Unterprozesse.

Messung auf dem vorhandenen macOS/ARM64-Bestand 2023 bis 2026:
131.049 Datensaetze, darunter 96.136 PDF-Seiten, 284.331 Woerter
und ein Index von rund 609 MiB.

| Messgroesse | Ergebnis |
| --- | --- |
| Warme Wortsuche, sieben Anfrage-/Filterkombinationen, je 300 Wiederholungen | p95 0,09 bis 1,92 ms |
| Phrase `"Landeshauptstadt Potsdam"`, 300 Wiederholungen | p95 1,10 ms |
| Praefix `wohn*`, 300 Wiederholungen | p95 0,47 ms |
| `--year 2024 'Schule OR wohn*'`, warme Suche, 300 Wiederholungen | p95 0,19 ms |
| Neuer Prozess: Index laden und validieren, Dateicache nicht geleert | 0,35 bis 0,37 s |
| Maximaler residenter Suchspeicher | rund 614 MiB |

Die Wortmessungen enthalten Schule, Haushalt, Verkehr, Wohnungen,
Potsdam und das haeufige Wort und, mit Jahr-/Typfilter beziehungsweise
Jahr-/Organisationsfilter und jeweils Top 20. Das vereinbarte warme
p95-Ziel unter 10 ms ist damit fuer diese Auswahl erreicht, nicht fuer
jede denkbare Anfrage oder Hardware garantiert.
Der Benchmarktreiber gibt die Einzelwerte als JSON aus.

Die erste Optimierungsrunde mit ARM-SHA beschleunigte den vollstaendigen
Einzelaufruf mit `--year 2024 'Schule OR wohn*'` von 1,62 auf
0,42 Sekunden, vor allem beim Programmstart.
Die anschliessende Suchkern-Optimierung senkte dessen warme p95 von
1,30 auf 0,19 ms (rund Faktor 6,7). Fuer Schule mit Jahr-/Typfilter
sank sie von 2,83 auf 0,14 ms, mit Jahr-/Organisationsfilter von 3,88
auf 0,16 ms. Das sehr haeufige Wort und mit Jahr-/Typfilter verbesserte
sich von 3,71 auf 1,92 ms.
Der vollstaendige Einzelaufruf benoetigte zuletzt im Median
0,368 statt 0,420 Sekunden, jeweils fuenf abwechselnde Messpaare.
1.584 Vorher-/Nachher-Vergleiche mit verschiedenen Filtern,
Boolean-Ausdruecken, Phrasen, Praefixen und Top 100 lieferten
bytegleiche Text- und JSON-Ausgaben, einschliesslich Reihenfolge,
Belegen und Gesamttrefferzahlen.

Ein schnellerer Suchkern beseitigt also nicht die weiterhin dominierenden
Ladekosten einzelner Prozesse; `--batch` amortisiert diese ueber mehrere
Anfragen.

Der lokale HTTP-Dienst wurde zusaetzlich mit 100 warmen Top-20-Anfragen
fuer `Schule OR wohn*`, Jahr 2024, auf demselben Bestand vermessen:
p50 rund 0,73 ms, p95 rund 1,14 ms. Diese Messung schliesst
HTTP-Verbindungsaufbau, Suche, JSON-Ausgabe und Antwortempfang ein,
nicht das anfaengliche Indexladen oder die Darstellung im Browser.
Die Antworten waren bytegleich zur CLI-Ausgabe. Das ist eine lokale
Einzelclient-Messung, kein Lasttest mit mehreren gleichzeitigen Nutzern.

## Tests

Die [Suchtests](../tests/search.mjs) pruefen AND/OR/NOT einschliesslich
Komplementen, Phrasen, Praefixe, Unicode, Metadatenfilter und gemeinsame
Drucksachenkontexte. Hinzu kommen wiederverwendete Batchpuffer,
vollstaendige Seitennavigation ohne doppelte Treffer, Datumsreihenfolge
einschliesslich fehlender Daten und Drucksachentitel passend zu Filtern,
Inhaltsabruf, Benchmark-Ausgabe und die Ablehnung beschaedigter Indexheader,
Nutzdaten oder ungueltiger Zeichenketten. Produktivarchive werden dabei
nicht beschrieben. Aufruf und Plattformhinweise stehen unter
[Tests](../README.md#tests).

Die [Webtests](../tests/search-web.mjs) vergleichen API-Antworten
byteweise mit CLI-Antworten, auch bei wechselnden Filtern. Sie pruefen
eingebettete UI-/Statusseiten, die exakte Indexgroesse, Inhaltsabruf,
Sortierung und Seitenparameter, HEAD, Eingabefehler, Host-/Origin-
Pruefung, Verbindungsgrenzen, partielle Anfragen und Zeitlimits.
Vorgangstests decken kanonische gegenueber eingebetteten Metadaten,
Untervorgaenge, zyklische Beziehungen, beide Schreibweisen von
`agendaItem`/`agendaitem`, Sitzungsdokumente, fehlende/geloeschte
Objekte und nicht zum Vorgang gehoerende Tagesordnungspunkte ab.
Der lokale Testdienst wird danach beendet.

### Fachliche MCP-Abnahme (M0)

Die [Abnahmetests](../tests/search-acceptance.mjs) verwenden ein kleines,
synthetisches [Buergerfragen-Testarchiv](../tests/fixtures/citizen-questions.mjs).
Sie testen die echten Programme ueber lokale Prozesse und Loopback-HTTP:

```sh
make test-mcp-acceptance
```

Diese separate Abnahme steht nach M4 bei **15/15 gruen**.
Exitcode 1 bedeutet fehlgeschlagene
fachliche oder Leistungs-Abnahme; Exitcode 2 einen Infrastruktur-/Werkzeugfehler.
Diese Codes gelten fuer den Node-Runner; GNU make meldet ein fehlgeschlagenes
Target unabhaengig davon mit seinem eigenen Exitcode 2.
Fehler werden nicht als erwartete Erfolge oder uebersprungene Tests maskiert.
Die bisherige Regression unter `make test` bleibt davon getrennt.

Geprueft werden falsche Paper-Zuordnungen ueber Sitzungspakete, legitime
gemeinsame Direktanlagen, Filter im selben Paper, Referenzjahr gegenueber
Dokumentjahr, Sitzungsdaten und Zeitzonen einschliesslich Zeitumstellung,
Antwortgroesse unter 20.000 HTTP-Body-Bytes, unpassende eingebettete
Tagesordnungspunkte und unabhaengiger Kalenderimport. Gegenpruefungen erhalten
widerspruechliche Datums-/Typangaben und explizit fehlende Beziehungen.
Die gemeinsame M3-Semantik normalisiert belegte Zeiten fuer Filter und
Sortierung; sie ist noch keine Dokumentrollenklassifikation oder belastbare
Bearbeitungszeitstatistik. `tests/search-semantics.mjs` prueft zusaetzlich
Nanosekunden, inklusive Quell-Lokaltage, Filter-only-Seitennavigation,
ungueltige/unbekannte Zeiten, mehrdeutige Sitzungskontexte, Besitzer-Joins
und Dokumentfassungsgruppen. Web-/MCP-Tests vergleichen die neuen
Filterantworten mit der CLI. `tests/search-research.mjs` prueft zusaetzlich
vollstaendige Beziehungsfortsetzungen, rekonstruierbare Elternverweise,
Mehrdeutigkeit trotz gekuerzter Belege, Statuskonflikte und belegte
Lueckenursachen einschliesslich langer nichtoeffentlicher IDs.

Die separate Vollindex-Praxispruefung wiederholt V2-Recherchen und misst
echte HTTP-Body-Bytes einschliesslich kompletter Pagination:

```sh
node tests/search-practice.mjs http://127.0.0.1:8080
```

Sie benoetigt den Potsdamer Vollindex: fuenf Sitzungstreffer, vollstaendige
Beziehungsnachladung, Fernwaermevorgang `2008128`, Tagesordnung `2001592`
und eine 20-ID-Projektion. Aelteres und modernes MCP werden getrennt
gemessen; die oeffentliche Installation wird dadurch nicht veraendert.

Stichtag, Zeitzone, Datumsrollen, Zaehleinheiten und unbekannte Werte stehen
in der Fixture. Bei einem Formatumbau darf der Runner angepasst werden;
fachliche Erwartungen sind kein Legacy-API-Vertrag. Der Testlauf beantwortet
nicht automatisch alle 25 Fragen eines externen Fragenkatalogs.

Der JSON-Bericht unter `build/acceptance/m0-baseline.json` enthaelt die einzelnen
Abnahmen, Binary-/Fixture-Pruefsummen, MCP-Aufrufzahlen und empfangenen
HTTP-Body-Bytes, Indexgroesse, gemessene Fixture-Bauzeit und warme CLI-Benchmarks.
Unter macOS wird auch der maximale RSS der gemessenen Prozesse erfasst.
Die Bytezahl umfasst JSON-RPC sowie Text- und Strukturausgabe, aber keine
HTTP-Header. Der Suchbenchmark schliesst Ausgabe und Transport nicht ein.

Optional laesst sich der vorhandene Gesamtindex nur lesend mitmessen:

```sh
node tests/search-acceptance.mjs \
  build/macos-aarch64/oparl-index build/macos-aarch64/oparl-search \
  build/macos-aarch64/oparl-scan \
  --corpus-index build/search/index.ops \
  --report build/acceptance/m0-baseline.json
```

Zusaetzlich misst `--corpus-archive downloads` einen vollstaendigen Neuaufbau
in einer temporaeren Ausgabedatei, mit zehn Minuten Zeitlimit. Erfasst werden
Bauzeit, maximaler RSS unter macOS, Coverage und Index-Pruefsumme. Ist auch
`--corpus-index` angegeben, wird Bytegleichheit beider Indexstaende ausgewiesen.
Der vorhandene Index wird nicht ersetzt; der temporaere Index wird entfernt.
Waehrenddessen darf kein Downloader dasselbe Archiv aktualisieren.

Mit `--citizen-report FILE` kann eine vorhandene Auswertung des urspruenglichen
Recherchelaufs als getrennte historische Baseline beigefuegt werden. Sie wird
nicht erneut ausgefuehrt und ist nicht mit der kleinen Fixture-Aufrufzahl
vergleichbar. Ohne `--corpus-archive` wird der Gesamtindex nicht neu gebaut;
seine Bauzeit wird dann ausdruecklich als ungemessen ausgewiesen.
Plattenkaltstarts sind nicht kontrolliert.

Alle Testdienste binden nur an `127.0.0.1` mit automatisch reservierten Ports
und werden anschliessend beendet. Auch der Downloader benutzt ausschliesslich
einen lokalen HTTP-Provider. Produktivarchive und oeffentliche Dienste werden
nicht veraendert.
