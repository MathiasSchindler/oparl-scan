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
| `--year YYYY` | Referenzjahr einer Drucksache, 2000 bis 2099, nicht das Datum der PDF |
| `--paper-type TYP` | Exakter Drucksachentyp, ohne Gross-/Kleinschreibung |
| `--organization TEXT` | Teilstring der zustaendigen Organisation, ohne Gross-/Kleinschreibung |
| `--kind TYP` | Objekttyp, z. B. `Paper`, `File`, `Page`, `Meeting` oder `Organization` |
| `--limit N` | Zahl der besten Treffer, 1 bis 100; Standard 20 |
| `--offset N` | Die ersten N sortierten Treffer ueberspringen, 0 bis 4294967295; Standard 0 |
| `--sort REIHENFOLGE` | `relevance` (Standard), `date-desc` oder `date-asc` |
| `--group MODUS` | `record` (CLI/API-Standard) oder `paper`: ein Treffer pro Vorgang |
| `--json` | Strukturierte Ausgabe statt lesbarer Textausgabe |
| `--batch` | Eine Suchanfrage je stdin-Zeile bei einmal geladenem Index |
| `--benchmark N` | 20 bis 100000 warme Wiederholungen einer Anfrage |
| `--show ID` | Vollstaendige archivierte Metadaten anhand einer ID abrufen |
| `--page N` | Zusammen mit `--show`: extrahierten PDF-Seitentext abrufen; Seiten ab 1 |
| `--serve` | Lokale Weboberflaeche und JSON-API auf `127.0.0.1` starten |
| `--port N` | Mit `--serve`: TCP-Port, 1 bis 65535; Standard 8080 |
| `--help` | Kurze Kommandozeilenhilfe |

Einzelne Suchanfrage, `--batch`, `--show` und `--serve` sind alternative
Betriebsarten. `--benchmark` gehoert zu einer einzelnen Suchanfrage
und kann nicht mit `--batch` oder `--show` kombiniert werden.
Im Webmodus werden Filter pro HTTP-Anfrage gesetzt; CLI-Filter und
`--json` sind dort nicht zulaessig.

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
Potsdamer Drucksachenbestand.

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
Die relevanteste passende Fundstelle liefert Ausschnitt, Quelle und PDF-Seite.
Der Gruppenwert ist der beste Einzelwert, keine Summe mit einem Bonus fuer
lange PDFs. `matchCount` zaehlt passende Datensaetze, nicht einzelne
Wortvorkommen. Nicht zuordenbare Dateien werden pro Datei, andere Objekte
weiter einzeln ausgegeben. AND/Phrasen werden weiterhin innerhalb eines
Datensatzes ausgewertet, nicht ueber mehrere Seiten verteilt.

`--sort date-desc` zeigt neueste, `--sort date-asc` aelteste Datensaetze
zuerst. Massgeblich ist der Kalendertag in `relatedDate`, sonst `date`:
bei PDF-Seiten also normalerweise das Datum der passenden Vorlage,
nicht Download-, Aenderungs- oder PDF-Erstellungszeit. Bei mehreren
Vorlagen gilt der ausgegebene, zu den Filtern passende Kontext.
Fehlende Datumsangaben stehen in beiden Richtungen zuletzt;
bei gleichem Datum entscheidet die Relevanz.

```sh
# Zweite Seite mit 50 Treffern, nach Datum absteigend.
build/macos-aarch64/oparl-search --year 2024 --sort date-desc \
  --limit 50 --offset 50 'Schule OR wohn*'
```

`--json` liefert ein Objekt mit:

- `total`: exakte Zahl passender Datensaetze beziehungsweise Vorgangsgruppen.
- `group`: `record` oder `paper`.
- `incomplete`: bekannte Archiv- oder Extraktionsluecken.
- `hits`: die besten Treffer bis zum gewaehlten Limit.

Jeder Treffer enthaelt `id`, `kind`, `name`, `reference`, `date`,
`paperType`, `organization`, `source`, `page`, `warnings`, `matchCount` und `snippet`.
Datei-/PDF-Seitentreffer enthalten ausserdem `documentUrl`, sofern
die archivierten File-Metadaten eine nicht leere `downloadUrl` oder
ersatzweise `accessUrl` nennen. Die URL wird nicht aus der OParl-ID
oder dem lokalen Dateinamen erraten.
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
Die Antwort liefert keine PDF-Binaerdaten; `source` verweist auf
die lokal archivierte Quelle, `documentUrl` auf das Originaldokument.

## Metadaten und Seitentexte abrufen

```sh
# Die ID aus einem vorherigen Suchtreffer uebernehmen.
build/macos-aarch64/oparl-search --show "$OPARL_ID" --json
build/macos-aarch64/oparl-search --show "$FILE_ID" --page 2 --json
```

Ohne `--page` liefert `--show` das archivierte JSON-Objekt.
Mit `--page` wird der extrahierte Seitentext des File-Objekts
abgerufen, nicht die visuelle PDF-Seite.

Die strukturierte Antwort enthaelt `id`, `page`, `warnings`,
`incomplete`, `source`, `text` und `metadata`. Fuer unzugeordnete
lokale Dokumente ist `metadata: null`. Ohne `--json` erscheinen
das rohe Metadaten-JSON beziehungsweise der Seitentext.
Nicht vorhandene IDs oder Seiten fuehren zu einem Fehler.

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
Die Sortierung kann zwischen Relevanz, neuestem und aeltestem Datum
wechseln; ihre Bedeutung wird neben der Auswahl erklaert.
Sortierungs- oder Seitengroessenwechsel beginnen wieder auf Seite 1.

PDF-Treffer zeigen den Titel ihrer zugehoerigen Vorlage als Ueberschrift,
sofern vorhanden. Dokumentnamen wie "Sammeldokument oeffentlich",
PDF-Seitennummer, Drucksachenkennzeichen, Typ und Datum bleiben separat
sichtbar. Titel und "Vorgang ansehen" fuehren zur lokalen Vorgangsseite.
"PDF oeffnen" beziehungsweise "Originaldokument oeffnen" fuehrt
direkt zur archivierten Original-URL, bei Potsdam normalerweise
auf `egov.potsdam.de`, in einem neuen Tab/Fenster. OParl-JSON-Links
stehen in den Metadatendetails, nicht anstelle des Dokumentlinks.
Lange lokale Quellenpfade und Extraktionswarnungen stehen
in den aufklappbaren Details. Die Websuche verwendet standardmaessig
`group=paper`: ein Treffer pro Vorgang mit Hinweis auf weitere passende
Fundstellen. Ausschnitte fassen aufeinanderfolgende Leerraeume zusammen.
Auf grossen Bildschirmen stehen Filter neben
den Ergebnissen; auf kleinen darueber, mit ausklappbaren Filtern.

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
  soweit diese Daten im Index vorhanden sind. Beschlussdateien,
  Einladungen und Protokolle werden ebenfalls verlinkt.
- Originalvorgang im RIS ueber dessen archiviertes `web`-Feld,
  Metadatendetails und die entsprechende JSON-Ausgabe.

Die HTML-Ansicht benoetigt wie die Suche JavaScript. Dieselben Daten
stehen unabhaengig davon unter `/api/paper?id=OPARL_ID` als JSON bereit:

```sh
curl --get http://127.0.0.1:8080/api/paper \
  --data-urlencode 'id=https://www.potsdam.sitzung-online.de/oparl/papers/2002374'
```

Die Antwort hat `id` fuer den angefragten Vorgang, `incomplete` und
eine nach ID deduplizierte Liste `objects`. Jedes Objekt enthaelt
`id`, `kind`, `indexed`, `source`, `warnings` und `metadata`.
`metadata` sind die unveraenderten OParl-Metadaten; die Beziehungen
bleiben dort erhalten. `indexed: true` bedeutet, dass ein eigener
Datensatz im geladenen Index gefunden wurde, nicht dass die PDF
vollstaendig archiviert oder fehlerfrei als Text erkannt wurde.

Eigene Indexdatensaetze haben Vorrang vor moeglicherweise veralteten
eingebetteten Angaben. Fehlt ein eigener Datensatz, bleiben eingebettete
Angaben als solche sichtbar (`indexed: false`); bei einer blossen
unaufgeloesten ID ist `metadata: null`. Solche Luecken setzen
`incomplete: true`. Geloeschte eingebettete Objekte werden gekennzeichnet,
nicht als aktive Dokumente verlinkt. Es gibt keine erfundenen
PDF-URLs und keine Live-Nachladeanfragen an das RIS.

Aufgeloest werden die expliziten Verknuepfungen des angefragten Papers
sowie dessen Beratungen, Tagesordnungspunkte, Sitzungen, Gremien und
zugehoerige Dateien. Untergeordnete oder verwandte Papers werden nicht
rekursiv zu einem unbeschraenkten Gesamtgraphen aufgefaltet; ihre
eigenen Seiten erschliessen die naechste Ebene. Andere Tagesordnungspunkte
derselben Sitzung werden nicht als Teil dieses Vorgangs ausgegeben.
Die rohen Sitzungsmetadaten bleiben dabei unveraendert. Es werden keine
fehlenden Gegenbeziehungen oder Beratungen hinzuerfunden.
Zyklen und mehrfach verwendete Dateien erzeugen keine doppelten Objekte.
Bei mehr als 4096 aufgeloesten Objekten wird die Anfrage mit Fehler
abgelehnt statt still gekuerzt.

Die Vorgangsansicht arbeitet mit vorhandenen Indizes; ein Neuaufbau
ist fuer diese Funktion nicht erforderlich.

### Indexstatus

Die verlinkte Unterseite <http://127.0.0.1:8080/health> zeigt die
Groesse der geladenen Indexdatei in MiB und Bytes, Indexspeicher,
Datensatz-/Begriffszahlen, PDF-Seiten sowie Seiten mit Extraktionswarnungen
und OCR-Bedarf. Der Indexspeicher umfasst Indexdaten und Beziehungstabellen,
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

### JSON-API

Alle Endpunkte sind nur lesend und unterstuetzen `GET` und `HEAD`.
Antworten sind UTF-8-JSON; `HEAD` liefert keinen Antwortkoerper.

| Endpunkt | Parameter | Antwort |
| --- | --- | --- |
| `/api/status` | Keine | `records`, `terms`, `indexBytes`, `indexMemoryBytes`, `pages`, `warningPages`, `ocrPages`, `incomplete` des geladenen Indexes |
| `/api/search` | `q` erforderlich; optional `year`, `paperType`, `organization`, `kind`, `limit`, `offset`, `sort`, `group` | Dieselbe JSON-Struktur wie die CLI-Suche |
| `/api/object` | `id` erforderlich; optional `page` ab 1 | Dieselbe JSON-Struktur wie `--show ID --json`, mit `page` wie `--show ID --page N --json` |
| `/api/paper` | Nur `id` erforderlich, muss ein indexiertes Paper sein | Aufgeloester Vorgang mit `id`, `incomplete`, `objects` |

Werte werden als URL-kodierte Queryparameter uebergeben. `+` wird zu
einem Leerzeichen, Prozentkodierung muss gueltiges UTF-8 ergeben.
Doppelte, unbekannte oder ungueltige Parameter werden abgelehnt,
nicht stillschweigend ignoriert. Grenzen und Bedeutung der Suchfilter
entsprechen der CLI; `paperType` entspricht `--paper-type`.
Ohne `limit` gilt fuer direkte API-Anfragen wie bei der CLI weiterhin
20; die Weboberflaeche fordert standardmaessig 50 an.
`offset` zaehlt ab 0; auch jenseits des letzten Treffers bleibt
`total` exakt und `hits` ist leer. `sort` akzeptiert `relevance`,
`date-desc` und `date-asc`.
`group` akzeptiert `record` und `paper`. CLI und direkte JSON-API bleiben
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
Weglassen von `page` abgerufen. Original-PDFs werden nicht lokal
ausgeliefert oder durch den Dienst geleitet: Die Oberflaeche verlinkt
ihre archivierte Original-URL. `source` bleibt ein lokaler Quellenpfad,
kein Download-Endpunkt.

Fehlerantworten haben die Form `{"error":"Beschreibung"}`:

| HTTP-Status | Bedeutung |
| --- | --- |
| `400` | Ungueltige Anfrage, Parameter oder Suchsyntax |
| `403` | Nicht erlaubter Host oder herkunftsfremde API-Anfrage |
| `404` | Unbekannter Endpunkt oder nicht vorhandene ID/Seite |
| `405` | Nicht erlaubte Methode; nur `/mcp` erlaubt POST |
| `408` | Zeitlimit fuer die vollstaendige Anfrage ueberschritten |
| `431` | Anfrageheader einschliesslich URL groesser als 64 KiB |
| `500` | Interner Fehler, etwa fehlender Anfragespeicher |
| `503` | Alle Verbindungsplaetze belegt |

Fehler erscheinen zusaetzlich auf stderr. Abgebrochene Clients
beenden nicht den Suchdienst. Ein unvollstaendiger Index bleibt
durchsuchbar und wird weiterhin mit `incomplete: true` gekennzeichnet.

### HTTP-MCP

Der bestehende Dienst stellt unter `/mcp` einen **Streamable-HTTP**-Transport
bereit, lokal beispielsweise `http://127.0.0.1:8080/mcp`, auf dem
bereitgestellten Server `http://178.254.23.157:8080/mcp`.
Es gibt keinen zusaetzlichen Port oder Prozess und keine MCP-SDK-
Laufzeitabhaengigkeit.

| Werkzeug | Argumente | Ergebnis |
| --- | --- | --- |
| `search` | `q`; optional dieselben Filter wie `/api/search`, einschliesslich `group` | Suchantwort; Standard 20 Treffer und `group=paper` |
| `get_object` | `id`; optional `page` ab 1 | Originalmetadaten oder extrahierter PDF-Seitentext |
| `get_paper` | `id` eines Papers | Aufgeloester Vorgang wie `/api/paper` |
| `get_index_status` | Keine | Indexgroesse, Zaehler und Abdeckung wie `/api/status` |

Alle Werkzeuge sind nur lesend und greifen ausschliesslich auf den geladenen
Index zu, nicht auf beliebige Dateien oder URLs. Erfolgreiche Aufrufe liefern
dieselben Daten in `structuredContent` und als JSON im Textinhalt.
Unbekannte Werkzeuge und fehlerhafte RPC-Nachrichten erzeugen JSON-RPC-Fehler;
ungueltige Werkzeugargumente, Suchsyntax und fehlende Objekte liefern
`isError: true` mit Beschreibung statt erfundener Ergebnisse.

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
Die PDF-Dateien werden weiterhin beim Originalanbieter geoeffnet.
Fuer den Suchdienst genuegt der selbststaendige Index mit Metadaten und
Volltexten; die Downloadarchive muessen nicht uebertragen werden.
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
CLI und testet Vorgangsseiten, JSON, HEAD sowie die Host-/Origin-Grenzen:

```sh
node tests/search-deployment.mjs http://178.254.23.157:8080/ \
  build/macos-aarch64/oparl-search build/search/index.ops
```

Diese optionale Pruefung verwendet den realen Potsdamer Index mit Daten
fuer 2024 und 2026; sie laeuft nicht als Teil der kleinen Fixture-Tests.

Fuer eine Aktualisierung den neuen Index zunaechst unter einem anderen
Dateinamen uebertragen, SHA-256 mit dem lokalen Original vergleichen,
dann auf demselben Dateisystem atomar nach `index.ops` umbenennen und
`systemctl restart oparl-search.service` ausfuehren. Den Abdeckungsbericht
ebenfalls aktualisieren. Den Suchindex weiterhin lokal mit `oparl-index`
bauen; der Server laedt nicht automatisch neue RIS-Daten nach.

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
