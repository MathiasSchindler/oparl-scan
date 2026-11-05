# oparl-scan

[Projektuebersicht und Build](../README.md) |
[oparl-index](oparl-index.md) |
[oparl-search](oparl-search.md)

`oparl-scan` archiviert und aktualisiert oeffentliche OParl-Metadaten
und Dokumentdateien. Der erste unterstuetzte Endpunkt ist Potsdam:

<https://www.potsdam.sitzung-online.de/oparl/system>

Das Programm archiviert das System, alle veroeffentlichten Koerperschaften und deren Sammlungen von
Organisationen, Personen, Sitzungen, Mitgliedschaften, Orten, Wahlperioden,
Tagesordnungspunkten, Drucksachen, Beratungen und Dateien. Der Download der
eigentlichen Dokumentdateien kann abgeschaltet werden. Die beiden
Koerperschaften werden aus der API ermittelt, nicht fest im Code vorgegeben.

Die erlaubten Produktionshosts und URL-Pfade sowie die Jahrgangsauswahl sind
derzeit auf Potsdam abgestimmt. Weitere Endpunkte brauchen ausdrueckliche
Unterstuetzung; allein eine andere Angabe fuer `--system` schaltet keine
beliebigen Hosts frei. Der Projektname beschreibt das allgemeinere Ziel,
nicht die bereits vollstaendige Unterstuetzung aller OParl-Implementierungen.

## Schnellstart

Alle Befehle werden im Wurzelverzeichnis des Repositorys ausgefuehrt.
Build und Plattformvoraussetzungen stehen in der [Projektuebersicht](../README.md).

```sh
# Vollstaendiges Archiv: Metadaten beider Koerperschaften und Dokumentdateien.
build/macos-aarch64/oparl-scan --output downloads

# Nur Metadaten.
build/macos-aarch64/oparl-scan --output downloads --metadata-only

# Vorgaenge des Jahrgangs 2025, einschliesslich spaeterer Beratungen und Dateien.
build/macos-aarch64/oparl-scan --year 2025 --output downloads/2025

# Bei Zugriffsverweigerungen einzelner Dokumente mit den anderen fortfahren.
build/macos-aarch64/oparl-scan --year 2025 --output downloads/2025 \
  --skip-unavailable-files

# Zusaetzlich vollstaendige Downloads mit falscher OParl-Groesse akzeptieren.
# Denselben Aufruf fuer eine gezielte Aktualisierung erneut ausfuehren.
build/macos-aarch64/oparl-scan --year 2025 --output downloads/2025 \
  --skip-unavailable-files --accept-size-mismatch

# Kleiner Live-Test, absichtlich unvollstaendig: Exitcode 2.
build/macos-aarch64/oparl-scan --output build/live-smoke \
  --page-size 2 --max-pages 1 --max-files 1
```

## Optionen

| Option | Bedeutung und Standard |
| --- | --- |
| `--output VERZEICHNIS` | Archivverzeichnis; Standard `downloads` |
| `--system URL` | OParl-System; Standard der oben genannte Potsdamer Endpunkt |
| `--year YYYY` | Referenzjahr 2000 bis 2099; ohne Option alle Sammlungen |
| `--metadata-only` | Keine Dokumentdateien herunterladen |
| `--skip-unavailable-files` | Verweigerte Dokumente protokollieren und fortfahren |
| `--accept-size-mismatch` | Vollstaendige Uebertragungen trotz falscher OParl-Groesse akzeptieren |
| `--refresh` | Vollstaendige Abrufe erzwingen; fuer normale Aktualisierung nicht noetig |
| `--page-size N` | Objekte je Seite, 1 bis 100; Standard 100 |
| `--max-pages N` | Seiten je Sammlung begrenzen; Standard unbegrenzt |
| `--max-files N` | Neue Dokumentdownloads je Aufruf begrenzen; Standard unbegrenzt |
| `--delay-ms N` | Pause je Zielhost und Port; Standard 500 ms |
| `--workers N` | Gleichzeitige Anfragen, 1 bis 2; Standard 2, hoechstens eine je Ziel |
| `--timeout-ms N` | Zeitlimit fuer TLS und Lesen; Standard 120000 ms |
| `--ca-file DATEI` | Vertrauenswuerdige CA-Zertifikate als PEM-Buendel |
| `--color MODUS` | `auto`, `always` oder `never`; Standard `auto` |
| `--help` | Kurze Kommandozeilenhilfe |

Unter Linux und Windows den passenden Programmpfad einsetzen.
Auf allen Plattformen Schraegstriche `/` in Pfaden verwenden.
Uebergeordnete Verzeichnisse des Ausgabeordners duerfen entsprechend der
newos-Richtlinie zur Verzeichniserstellung keine symbolischen Links sein.

Standardmaessig laufen hoechstens **zwei Anfragen gleichzeitig**, jedoch
niemals zwei zum selben Zielhost und Port. Zwischen dem Ende einer Anfrage
und dem Beginn der naechsten am selben Ziel liegen mindestens 500 ms.
Bei voruebergehenden Fehlern erfolgen bis zu drei Versuche.
Numerische `Retry-After`-Werte bis 300 Sekunden werden beruecksichtigt;
andere Werte fuehren zu einer ausdruecklichen Fehlermeldung. Weiterleitungen
sind auf die oeffentliche API und `egov.potsdam.de/public/` beschraenkt.
Zugangsdaten oder nichtoeffentliche Bereiche werden nicht verwendet.

### Parallelitaet und Serverpausen

Ein Dokumentdownload von `egov.potsdam.de` kann sich mit einem
Metadatenabruf von `www.potsdam.sitzung-online.de` ueberschneiden.
`--delay-ms N` gilt dabei getrennt je Zielhost und Port; Zeit, die
bereits mit Arbeit an einem anderen Ziel verbracht wurde, zaehlt als Pause.
Auch jede Weiterleitung muss erst den freien Platz und die Pause ihres
Zielhosts abwarten. Der bisherige Platz wird davor freigegeben.
HTTP 429, voruebergehende Server- und Transportfehler sowie numerisches
`Retry-After` verschieben die naechste zulaessige Anfrage an das betroffene
Ziel, nicht nur den naechsten Versuch derselben URL.

Mit `--workers 1` laesst sich die Ausfuehrung auf eine Anfrage insgesamt
begrenzen. Der Standard ist `--workers 2`; groessere Werte sind nicht
erlaubt. Cache, Archividentitaet, Dateiformate und Wiederaufnahme sind
unabhaengig von dieser Einstellung. Es darf trotzdem nur ein Prozess
in ein Ausgabeverzeichnis schreiben.

Die Metadatensuche bleibt geordnet. Bis zu 32 bereits entdeckte,
noch nicht heruntergeladene Dokumente werden mit kleinen
Auftragsbeschreibungen vorgemerkt. Ein Abruf kann mit einem Dokument
eines anderen Ziels zusammen ausgefuehrt werden; am Ende werden alle
restlichen Auftraege abgearbeitet. Dokumentinhalte bleiben gestreamt,
nicht im Arbeitsspeicher gesammelt. `--max-files` beruecksichtigt auch
vorgemerkte Auftraege, damit die Grenze nicht durch Parallelitaet
ueberschritten wird. Doppelte Datei-IDs werden bereits beim Vormerken
erkannt.

Das ist eine begrenzte Ueberlappung in Zweiergruppen, kein unbeschraenkter
Hintergrunddownload: Fehlt ein unabhaengiger Auftrag, bleibt es bei einer
Anfrage. Insbesondere wird der anfaengliche Drucksachen-Suchlauf auf
einem einzigen Host dadurch nicht parallelisiert.

Verwendet wird der vorhandene newos-Taskpool ohne libc oder pthreads.
Der newos-I/O-Loop reicht hier noch nicht aus, weil die TLS-Schnittstelle
blockiert. Deshalb werden hoechstens zwei abgeschlossene Netzwerkauftraege
zusammengefuehrt, bevor der Koordinator Archivzaehler und Berichte aendert.
Auch bei einem fatalen Fehler wird ein bereits gestarteter Partnerauftrag
zu Ende abgewartet; erfolgreiche Dateien bleiben fuer die Wiederaufnahme
erhalten. Nicht gestartete Auftraege werden dann verworfen.
Gemeinsamer TLS-ClientHello-Speicher, Namensaufloesung und Diagnoseausgabe
werden geschuetzt; TLS-Handshakes und HTTP-Empfang behalten getrennte
Stackbereiche.

Native Parallelitaet steht mit dem eingebundenen newos unter macOS/ARM64
und Linux/x86-64 zur Verfuegung. Linux/ARM64 hat noch kein natives
Worker-Backend. Unter Windows verwendet OParl-Scan ebenfalls die serielle
Ausfuehrung, weil die eingebundene Windows-Dateideskriptortabelle noch nicht
fuer parallele Socket-Zuweisung ausgelegt ist. In beiden Faellen wird bei
einer sonst parallelen Zweiergruppe eine Warnung ausgegeben, statt unsichere
Parallelitaet oder eine fremde Laufzeit zu verwenden.

Unterschiedliche Hostnamen garantieren keine unabhaengige Infrastruktur.
Auch kann die durchschnittliche Anfragerate steigen, weil zusaetzliche
Ruhezeiten waehrend fremder Downloads entfallen. Die getrennten Limits
begrenzen diese Last, machen sie aber nicht automatisch identisch mit
einem vollstaendig nacheinander ausgefuehrten Download.

Einzelne grosse Listenseiten koennen erheblich langsamer antworten als der
System-Endpunkt. Bei einem Live-Test brauchte beispielsweise Drucksachenseite
210 mit 100 Eintraegen rund 47 Sekunden bis zum ersten Antwortbyte.
Das fruehere Standardzeitlimit von 30 Sekunden reichte dafuer nicht aus.
Der Standard betraegt jetzt 120 Sekunden; er kann mit `--timeout-ms`
weiter angepasst werden. Bei Fehlern vor dem HTTP-Status werden Senden
und Lesen getrennt gemeldet. Lesefehler nennen zusaetzlich die verstrichene
Lesezeit, das konfigurierte Zeitlimit und den verfuegbaren Transportfehler.
Die TLS-Helfer koennen dabei nicht jeden Verbindungsabbruch eindeutig von
einem Zeitlimit unterscheiden.

Zur schonenderen Wiederaufnahme des Jahrgangs 2024 beispielsweise denselben
Archivordner und die bisherigen Pruefoptionen verwenden, ohne `--refresh`:

```sh
build/macos-aarch64/oparl-scan --year 2024 --output downloads/2024 \
  --timeout-ms 120000 --delay-ms 1500 \
  --skip-unavailable-files --accept-size-mismatch
```

Die beiden letzten Optionen nur beibehalten, wenn diese Ausnahmen auch
bisher bewusst gewaehlt wurden. Bereits vollstaendige Metadaten und
gueltige Dokumente werden wiederverwendet. Wiederholungen bleiben auf
drei Versuche begrenzt; ein nicht antwortender Server fuehrt weiterhin
zu einem ausdruecklichen Fehler und einem unvollstaendigen Archiv.

### Farben und Diagnosemeldungen

Warnungen erhalten ein fettgelbes Praefix, Fehler ein fettrotes Praefix.
Ein unvollstaendiger Abschluss wird ebenfalls fettgelb hervorgehoben.
Normale Fortschrittsmeldungen wie `metadata:` und `document:` bleiben
ungefaerbt. Die englischen Meldungstexte, Optionsnamen und JSON-Feldnamen
bleiben unveraendert.

Die Farben verwenden ausschliesslich die vorhandenen newos-Helfer:

- `--color auto`: Standard; Farben bei Terminalausgabe auf stderr,
  normalerweise keine ANSI-Codes in umgeleiteten Logs.
- `--color always`: Farben bewusst auch bei Umleitung erzwingen.
- `--color never`: Farben immer abschalten, auch bei geerbten
  Umgebungsvariablen zur Farberzwingung.

Im automatischen Modus werden die newos-Konventionen fuer `NO_COLOR`,
`CLICOLOR`, `CLICOLOR_FORCE` und `TERM=dumb` beruecksichtigt.
`CLICOLOR_FORCE` kann Farben auch bei Umleitung erzwingen und hat dabei
Vorrang vor `NO_COLOR`; `--color never` verhindert dies zuverlaessig.
Textpraefixe wie `warning:` und `error:` bleiben auch ohne Farben sichtbar.
Archivdateien und JSON-Berichte erhalten keine Farbcodes.

```sh
# Garantiert farbcodefreies Protokoll.
build/macos-aarch64/oparl-scan --year 2025 --output downloads/2025 \
  --color never 2>downloads/2025.log
```

### Auswahl eines Drucksachenjahrgangs

`--year 2025` waehlt Kennzeichen mit dem Praefix **`25/`**, darunter
`25/SVV/...`, `25/OBR/...` und Ergaenzungen wie `25/SVV/0403-001`.
Die Auswahl richtet sich **nicht** nach dem Feld `date` der Drucksache,
nach Sitzungsterminen oder Dateizeitstempeln. Beispielsweise hat die echte
Drucksache `25/SVV/1216` ein Datum im Maerz 2026 und wird trotzdem erfasst.
Unterstuetzt werden die Jahrgaenge 2000 bis 2099.

Zunaechst liest das Programm die vollstaendigen Drucksachenlisten beider
Koerperschaften. Ein verlaesslicher serverseitiger Kennzeichenfilter ist
nicht bekannt: Der getestete Parameter `paperNumber` wird von dieser API
ignoriert. Dieser reine Metadaten-Suchlauf verhindert, dass spaet angelegte,
umnummerierte oder ausserhalb der Reihenfolge stehende Vorgaenge fehlen.
Die ungefilterten Originalantworten liegen getrennt unter `index/`.

Passende Drucksachen werden einzeln abgerufen. Hauptdateien, Anlagen,
uebergeordnete, untergeordnete und verwandte Drucksachen, Beratungen,
zugehoerige Tagesordnungspunkte, Beschlussdateien und Sitzungen werden
rekursiv verfolgt. Eine Deduplizierung anhand der Identitaeten verhindert
Zyklen. Auch verknuepfte Vorgaenge und Unterlagen spaeterer Jahre sind
enthalten. Neben `agendaItem` wird das nichtstandardisierte Potsdamer Feld
`agendaitem` unterstuetzt.

Bei Potsdamer Organisationsobjekten steht die OParl-Klassen-URI teilweise
abweichend vom Standard in `"Type"`, waehrend `"type"` den Wert `"at"` oder
`"gr"` enthaelt. Genau diese Kombinationen werden bei Validierung und
Verarbeitung als Organisationen erkannt. Das archivierte JSON bleibt
unveraendert; fehlende oder ungueltige Klassen-URIs und abweichende
Ressourcen-IDs fuehren weiterhin zu Fehlern.

Zu verknuepften Sitzungen werden Einladungen, Protokolle, Anlagen und
Organisations- beziehungsweise Ortsinformationen erfasst. Das originale
Meeting-JSON kann die gesamte Tagesordnung enthalten; sitzungsweite PDFs
koennen weitere Vorgaenge behandeln. Unbeteiligte Tagesordnungspunkte werden
jedoch nicht rekursiv expandiert oder einzeln heruntergeladen, weil sonst
ein einzelner Vorgang schliesslich das gesamte RIS nachziehen wuerde.
Organisations- und Personendatensaetze liefern Kontext, ohne rueckwaertige
Mitgliedschafts- oder Sammlungsverknuepfungen weiter zu verfolgen.

`selection.jsonl` verzeichnet IDs und Kennzeichen der ausgewaehlten
Ausgangsdrucksachen. Verknuepfte Folgedrucksachen koennen andere Jahrgaenge
haben. `archive.json` enthaelt unter anderem `referenceYear`,
`scannedPapers`, `selectedPapers` und `processedResources` und wird waehrend
des Laufs aktualisiert. `selectedPapers` zaehlt nur die Ausgangsdrucksachen,
nicht alle Kontextobjekte. Fuer jeden Jahrgang und jedes System ist ein
eigenes Ausgabeverzeichnis noetig. Eine Vermischung wird abgelehnt, ohne die
vorhandene Archividentitaet zu ersetzen.

### TLS und Zertifikatspruefung

HTTPS nutzt newos' gemeinsame TLS-1.3-Implementierung mit einem Rueckfall
auf TLS 1.2 fuer den Dokumentserver. Zertifikatskette, Hostname und
Gueltigkeit werden auf **jeder Plattform** ausdruecklich mit dem gemeinsamen
X.509-Code geprueft. Eine Option zum Abschalten dieser Pruefung gibt es nicht.

Das PEM-Buendel vertrauenswuerdiger Zertifikate wird ueber `--ca-file FILE`,
danach `SSL_CERT_FILE` oder den ersten verfuegbaren Standardpfad unter Unix
bestimmt. Unter Windows muss das Buendel per Option oder Umgebungsvariable
bereitgestellt werden; der Windows-Zertifikatsspeicher wird nicht verwendet.
Ein CA-Buendel ist Vertrauensmaterial, keine eingebundene Bibliothek.

`--timeout-ms` betraegt standardmaessig 120000 und begrenzt TLS-Operationen
und das Lesen von Antworten, nicht die Gesamtlaufzeit. DNS-Aufloesung und
TCP-Verbindungsaufbau verwenden newos' blockierende Plattform-API und
werden davon nicht begrenzt. HTTP ohne TLS ist nur fuer ein ausdruecklich
gewaehltes Testsystem auf `127.0.0.1` erlaubt.

### Archivstruktur, Aktualisierung und Wiederaufnahme

```text
downloads/
  archive.json
  missing-resources.jsonl                  # HTTP-404-Fehlstellen pro Lauf
  selection.jsonl                         # nur bei Jahrgangsauswahl
  index/<sha256-anfrage-url>.json          # ungefilterte Suchlaufseiten
  metadata/<sha256-anfrage-url>.json
  documents/<sha256-oparl-datei-id>-<bereinigter-originalname>
  cache/metadata-<sha256-anfrage-url>.json  # HTTP-Pruefdaten und Inhaltshash
  cache/document-<sha256-oparl-datei-id>.json
```

Die Metadatenseiten behalten das originale UTF-8-JSON mit Beziehungen,
Seitennavigation, Datei-IDs, Originalnamen, Download-URLs und Zeitstempeln.
Die Hashes sind kleingeschriebene hexadezimale SHA-256-Werte der
UTF-8-URL beziehungsweise -ID, keine Hashes des Dateiinhalts.
Dateinamen werden auf ASCII-Buchstaben, Ziffern, `.`, `_` und `-`
beschraenkt; der Originalnamenanteil ist auf 100 Bytes begrenzt.
IDs verhindern Kollisionen bei gleichen Dateinamen. `archive.json`
identifiziert das System und speichert Abschlussstatus und Zaehler pro Lauf.

Downloads werden in `.part`-Dateien gestreamt und erst nach erfolgreichem
Abschluss umbenannt. Metadaten werden vor der Veroeffentlichung validiert;
von OParl angegebene Dokumentgroessen werden geprueft. Dieselbe Datei-ID
wird pro Lauf nur einmal heruntergeladen, auch wenn sie in mehreren
Sammlungen eingebettet vorkommt.

**Jeder normale Aufruf aktualisiert das Archiv**, ohne zusaetzliche Option.
Der Drucksachen-Suchlauf liest die Sammlungen beider Koerperschaften erneut
ein und folgt den aktuellen Seitenlinks. Auch bereits bekannte verknuepfte
Objekte werden erneut geprueft. Neue Drucksachen, geaenderte Beziehungen
und spaeter hinzugekommene Dokumente werden so ermittelt, auch bei
abgeschlossenen Referenzjahrgaengen.

Gespeicherte Metadaten werden mit `If-None-Match` beziehungsweise
`If-Modified-Since` revalidiert, sofern die vorherige Antwort einen
`ETag` beziehungsweise `Last-Modified` geliefert hat. Nur eine passende
HTTP-304-Antwort erlaubt die lokale Wiederverwendung. Andernfalls wird
die neue Antwort validiert und veroeffentlicht. Ein Netzwerkfehler wird
nicht durch ungepruefte alte Metadaten ersetzt.

Dokumente mit solchen Pruefdaten werden zuerst per HEAD geprueft. Eine
304-Antwort oder dieselben Validatoren in einer HEAD-200-Antwort bestaetigen
den vorhandenen Inhalt, ohne die PDF erneut zu uebertragen. Bei geaenderten
Validatoren, anderer Ziel-URL, fehlender HEAD-Unterstuetzung oder fehlenden
Validatoren ist ein GET noetig. ETag hat Vorrang vor Last-Modified;
Pruefdaten gelten nur fuer die gespeicherte endgueltige URL, nicht fuer
eine andere Weiterleitungsadresse. Alle Pruefanfragen beachten die
normalen Hostpausen und Parallelitaetsgrenzen.

Die kleinen Dateien in `cache/` verbinden diese HTTP-Pruefdaten mit
Anfrage-URL, tatsaechlicher Groesse, HTTP-Rahmeninformation und SHA-256
des archivierten Inhalts. Vor einer bedingten Anfrage wird der lokale
Inhalt gehasht; auch Beschaedigungen bei unveraenderter Groesse fuehren
damit zu einem erneuten Abruf. Die Hashes in den Dokumentdateinamen
bleiben dagegen ID-Hashes. Ungueltige Pruefdatensaetze werden mit einer
Warnung verworfen und durch einen vollstaendigen Abruf ersetzt.

Bei bestehenden Archiven ohne diese Pruefdaten ist einmalig ein erneuter
Abruf noetig, um den aktuellen Serverstand verlaesslich mit den lokalen
Bytes zu verbinden. Danach koennen unveraenderte Dokumente ohne erneute
Inhaltsuebertragung bestaetigt werden. Server ohne brauchbare Validatoren
erfordern weiterhin vollstaendige Abrufe. Die Aktualitaetspruefung haengt
von korrekten HTTP-Validatoren des Servers ab; sie ist kein unabhaengiger
Nachweis, dass die API alle oeffentlichen RIS-Daten bereitstellt.

Zur Wiederaufnahme denselben Befehl erneut ausfuehren. Unvollstaendige
`.part`-Dateien werden neu begonnen, nicht byteweise fortgesetzt.
`--refresh` erzwingt weiterhin vollstaendige Abrufe ohne Cachepruefung,
ist aber fuer die normale Aktualisierung nicht erforderlich.
Angetroffene Metadaten und geaenderte Dokumente werden ersetzt; eine
Versionshistorie entsteht nicht. Alte, nicht mehr verknuepfte Dateien
werden nicht automatisch geloescht. Fuer einen getrennten neuen
Schnappschuss ein anderes Verzeichnis verwenden. Nie zwei Prozesse
gleichzeitig dasselbe Ausgabeverzeichnis verwenden lassen.

`newMetadataPages` zaehlt vollstaendig abgerufene Metadatenantworten,
`cachedMetadataPages` per HTTP 304 bestaetigte lokale Antworten.
`newDocuments` zaehlt vollstaendige Dokumentdownloads,
`cachedDocuments` ohne erneute Inhaltsuebertragung bestaetigte Dokumente.
Alle Zaehler gelten fuer den jeweiligen Aufruf, nicht fuer die gesamte
Archivhistorie. Ein erneuter Aufruf ist kein rein lokaler Cache-Abgleich
mehr: Metadatenabrufe und Dokumentpruefungen benoetigen Netzwerkanfragen.

Der lokale Integritaetsabgleich bleibt einstrangig. Fortschrittsstaende werden
hoechstens zweimal pro Sekunde geschrieben, nicht nach jeder gecachten
Ressource. Der anfaengliche unvollstaendige Status und der Abschlussstatus
werden immer sofort geschrieben. Laufende Zaehler koennen kurz nachlaufen;
die Wiederaufnahme stuetzt sich auf die Dateien, nicht auf den letzten
gespeicherten Zaehlerstand.

`--max-pages N` begrenzt Seiten **pro Sammlung**, einschliesslich der
Koerperschaftsliste. `--max-files N` begrenzt **neu heruntergeladene
Dokumente pro Aufruf**; gecachte Dokumente verbrauchen dieses Budget
nicht. Fuer eine vollstaendige Erfassung die Grenzen entfernen.

### Fehlende URLs (HTTP 404)

**HTTP 404 beendet den Lauf nicht.** Das gilt standardmaessig fuer
Metadaten und Dokumentdownloads, ohne zusaetzliche Option. Die rote
HTTP-Fehlermeldung bleibt sichtbar; das Programm verarbeitet die
uebrigen ermittelten Ressourcen weiter. Es speichert weder Fehlerseiten
als Dokumente noch Platzhalter als Metadaten.

`missing-resources.jsonl` protokolliert jede betroffene angefragte URL
einmal pro Lauf mit `url`, `kind` (`metadata` oder `document`) und
`status: 404`. Bei Weiterleitungen ist `url` die urspruenglich
angefragte URL; die HTTP-Diagnose nennt die URL der 404-Antwort.
`archive.json` zaehlt diese URLs in `missingResources` und bleibt
bei mindestens einer solchen Fehlstelle auf `complete: false`.
Der Abschluss wird als `INCOMPLETE (HTTP 404)` ausgegeben, mit Exitcode `3`.
Dieser hat Vorrang vor konfigurierten Grenzen; andere fatale Fehler
beenden den Lauf weiterhin mit Exitcode `1`.

Fehlt eine Sammlungsseite, ist deren naechster Seitenlink unbekannt.
Das Programm beendet deshalb nur diese Sammlung, ohne weitere
Seiten-URLs zu erfinden, und verarbeitet die anderen bekannten
Sammlungen und Ressourcen. Fehlt bereits das System oder die
Koerperschaftsliste, koennen keine weiteren Sammlungen ermittelt werden;
der Lauf endet ebenfalls mit protokollierter Fehlstelle, nicht erfolgreich.

Beim naechsten Aufruf werden fehlende Ressourcen erneut versucht,
waehrend gueltige Cachedateien revalidiert werden. Bereits archivierte
Daten bleiben erhalten, werden bei einer aktuellen 404-Antwort aber
nicht als erfolgreiche Antwort verwendet. Es ist kein
`--refresh` noetig. Der Bericht wird pro Lauf neu erstellt; behobene
Fehlstellen verschwinden daraus. Waehrend des Laufs entsteht
`missing-resources.jsonl.part`, die erst nach einem Durchlauf ohne
fatalen Fehler umbenannt wird. Bei Abbruch bleibt der Teilbericht
erhalten; ein alter veroeffentlichter Bericht gilt weiterhin fuer
den vorherigen abgeschlossenen Lauf.

### Dokumente mit verweigertem Zugriff

Einige oeffentlich gelistete Potsdamer Dokumente leiten auf `./noauth`
weiter. Das ist kein Dokumentdownload; dieser Authentifizierungsroute
wird niemals gefolgt. Standardmaessig bricht das Programm mit einer
ausdruecklichen Fehlermeldung ab.

Mit `--skip-unavailable-files` werden Dokumentantworten mit genau dieser
Weiterleitung oder HTTP 401/403 stattdessen in `unavailable-files.jsonl`
verzeichnet. Die uebrigen Downloads laufen weiter. Jede Zeile enthaelt
`id`, angefragte `url`, urspruenglichen `fileName` und
`reason: "access-denied"`. Es werden weder Platzhalter noch
Anmeldeseiten als Dokument gespeichert. Doppelte Datei-IDs werden pro
Aufruf nur einmal versucht und protokolliert. Andere Fehler, darunter
Zugriffsverweigerungen fuer Metadaten, unerlaubte
Weiterleitungen, TLS-Fehler und fehlerhafte Antworten, fuehren weiterhin
zum Abbruch.

`archive.json` enthaelt `skipUnavailableFiles` und `unavailableDocuments`.
Bei fehlenden Dokumenten bleibt `complete: false`, auch wenn der
Suchlauf beendet ist. Der Bericht wird bei jedem Aufruf neu erstellt;
fehlende Dateien werden erneut versucht, erfolgreiche Downloads normal
wiederverwendet. Werden alle zuvor gesperrten Dateien verfuegbar,
ist der naechste Bericht leer und das Archiv kann ohne `--refresh`
vollstaendig werden.

Waehrend des Laufs entsteht `unavailable-files.jsonl.part`. Nach einem
Durchlauf ohne fatalen Fehler wird sie umbenannt. Bei Unterbrechung oder
fatalem Fehler bleibt der Teilbericht erhalten. Ein eventuell noch
vorhandener veroeffentlichter Bericht beschreibt dann weiterhin den
vorherigen abgeschlossenen Durchlauf.

### Falsche OParl-Dateigroessen

Die Groessenpruefung bleibt standardmaessig streng. Manche Potsdamer
Eintraege geben selbst in frisch abgerufenen Metadaten eine falsche
Groesse an: Datei `2912324` nennt beispielsweise 83.930 Bytes,
der Dokumentserver liefert aber eine PDF mit 83.944 Bytes.

Mit `--accept-size-mismatch` wird die OParl-Groesse ausdruecklich als
unverbindlich behandelt, wenn die vollstaendige HTTP-Uebertragung durch
`Content-Length` oder gueltige Chunk-Kodierung unabhaengig bestaetigt
ist. Abgebrochene Uebertragungen bleiben Fehler. Eine abweichende Antwort
ohne eine dieser beiden Rahmeninformationen wird nicht akzeptiert.
Eine vollstaendig uebertragene Groessenabweichung wird nicht wie ein
voruebergehender Netzwerkfehler erneut versucht.

Akzeptierte Dateien werden normal gespeichert und in
`size-mismatches.jsonl` mit `id`, `url`, `fileName`, `expectedSize`
und `actualSize` verzeichnet. Das originale OParl-JSON wird nicht
veraendert. `archive.json` enthaelt `acceptSizeMismatch` und
`sizeMismatchedDocuments`. Akzeptierte Abweichungen sind keine fehlenden
Dateien und verhindern daher unter dieser ausdruecklich gewaehlten
Pruefregel keinen vollstaendigen Abschluss. Zugriffsfehlstellen und
konfigurierte Grenzen tun dies weiterhin.

Auch akzeptierte Groessenabweichungen koennen bei spaeteren Aufrufen
wiederverwendet werden: Inhaltshash, tatsaechliche Groesse, urspruengliche
HTTP-Rahmeninformation und Servervalidatoren muessen die Datei bestaetigen.
Eine beschaedigte oder geaenderte Datei wird erneut heruntergeladen.
Ohne diese Pruefdaten ist ebenfalls ein erneuter Abruf noetig.
Der Abweichungsbericht wird auch fuer bestaetigte Cachedateien pro Lauf
neu erstellt und folgt demselben `.part`-/Umbenennungsverfahren wie der
Bericht ueber nicht verfuegbare Dateien.

### Exitcodes und Grenzen

- `0`: Alle ermittelten Sammlungen wurden im gewaehlten Modus verarbeitet.
- `1`: Netzwerk-, TLS-, JSON-, Seitennavigations-, Dateisystem- oder
  Dokumentfehler.
- `2`: Wegen konfigurierter Grenzen absichtlich unvollstaendig.
- `3`: Durchlauf beendet, aber wegen HTTP 404 fehlen Ressourcen oder
  wegen Zugriffsverweigerungen fehlen Dokumente. Dies hat Vorrang
  vor Exitcode `2`; fatale Fehler liefern weiterhin `1`.

Im Modus `--metadata-only` bedeutet Vollstaendigkeit nur vollstaendige
Metadaten, nicht vollstaendige Dokumentdownloads. Dies wird in
`archive.json` festgehalten. Ein unterbrochener Lauf hinterlaesst
`complete: false`. OParl ist eine laufend veraenderliche API; ein Archiv
ist daher kein atomarer Datenbankschnappschuss zu einem einzigen Zeitpunkt.

Die Grenzen betragen 32 MiB pro Metadatenantwort, 4 GiB pro Dokument
und 64 Verschachtelungsebenen fuer JSON. Fehler beenden den Lauf,
abgesehen von HTTP 404 und den oben beschriebenen, ausdruecklich
aktivierten Ausnahmen. HTTP-Fehler und ungueltige Eingaben werden niemals durch
leere, scheinbar erfolgreiche Antworten ersetzt.

## Tests und Verifikation

Gemeinsame Test- und Buildhinweise stehen unter [Tests](../README.md#tests).
Die Downloadtests koennen mit der gesamten Testsuite ausgefuehrt werden:

```sh
make test
make check-deps
make TARGET=linux ARCH=x86_64 check-deps
make TARGET=windows ARCH=aarch64 check-deps
```

Die Parser-Tests laufen als freistehendes newos-Programm. Die
Integrationstests verwenden einen temporaeren lokalen Testserver.
Sie pruefen Seitennavigation, beide Koerperschaften, alle Sammlungstypen,
eingebettete doppelte Dokumente, HTTP-Chunk-Kodierung, Weiterleitungen,
Wiederaufnahme, reinen Metadatenbetrieb, Aktualisierung, ausdrueckliche
Grenzen, fehlerhafte und abgebrochene Antworten sowie Wiederholungen.
Langsame Antworten und Verbindungen ohne HTTP-Status werden getrennt
geprueft, einschliesslich Wiederaufnahme mit einem hoeheren Zeitlimit
und Revalidierung zuvor abgeschlossener Metadaten.
Hinzu kommen Jahrgangsauswahl, spaetere verknuepfte Vorgaenge und
Dokumente, Graph-Deduplizierung, abweichende Organisationsfelder,
Cache-Wiederverwendung und Archividentitaet.

Aktualisierungstests aendern zwischen normalen Aufrufen die
Drucksachenlisten und deren Seitenzahl, verknuepfte Vorgaenge, Dokumente
gleicher Groesse und Weiterleitungsziele. Sie pruefen bedingte Anfragen,
HEAD-Bestaetigung bei ignoriertem If-Modified-Since, GET-Rueckfall,
alte Archive ohne Pruefdaten, lokale Beschaedigungen gleicher Groesse,
ungueltige Pruefdatensaetze und unerwartete HTTP-304-Antworten.

Weitere Tests decken HTTP 404 fuer System, Koerperschaftsliste,
Sammlungen, Folgeseiten, verknuepfte Ressourcen und Dokumentdownloads
ab, einschliesslich Fortsetzung, Berichten, Deduplizierung und
Wiederaufnahme nach Behebung der Fehlstellen.
Weitere Tests decken Zugriffsfehlstellen und ihre Behandlung bei
Wiederaufnahme sowie Groessenabweichungen mit `Content-Length` und
Chunk-Kodierung ab. Sie pruefen Berichte, erneute Cachevalidierung
und das Ablehnen abgebrochener oder nicht ausreichend eingerahmter
Uebertragungen. Farbtests pruefen hervorgehobene Warnungen und Fehler,
ungefaerbte Fortschrittsmeldungen und die Farbmodi fuer umgeleitete Ausgabe.

Die Parallelitaetstests verwenden zwei unabhaengige lokale HTTP-Endpunkte.
Sie messen echte Ueberlappung von Metadaten und Dokumenten, hoechstens
zwei aktive Anfragen insgesamt, hoechstens eine je Ziel sowie die
konfigurierte und die standardmaessige Pause. Weiterleitungen in beide
Richtungen und eine andere URL am durch `Retry-After` pausierten Ziel
werden ebenfalls geprueft. Hinzu kommen bytegleiche Archive im seriellen
Modus, Queue-Grenzen, Downloadlimits, Fehlstellenberichte, gleichzeitige
Fehler und Wiederaufnahme.

Mit `OPARL_TEST_TLS_CERT` und `OPARL_TEST_TLS_KEY` laesst sich
`node tests/concurrency.mjs /absoluter/pfad/zu/oparl-scan` zusaetzlich gegen
zwei lokale HTTPS-Endpunkte ausfuehren. Die temporaere Testbescheinigung
muss fuer `127.0.0.1` gelten; sie wird als CA-Buendel uebergeben.
`OPARL_TEST_TLS12=1` beschraenkt den Dokument-Testserver auf TLS 1.2,
waehrend der Metadaten-Testserver TLS 1.3 verwendet. Der TLS-1.2-Testserver
erlaubt ausschliesslich fuer diese lokale Kompatibilitaetspruefung den
aelteren ClientHello des eingebundenen newos. Die Zertifikatspruefung des
Downloadprogramms bleibt unveraendert aktiv. Zertifikate und Testschluessel
werden nicht mitgeliefert.

Lokal unter macOS/ARM64 geprueft wurden native Ausfuehrung, lokale
Testdaten, zertifikatsgepruefte Live-Downloads von API-Daten und PDFs
sowie Cache-Wiederverwendung. Linux- und Windows-Builds fuer x86-64
und ARM64 werden plattformuebergreifend kompiliert und auf ihre
Laufzeitimporte geprueft; auf diesen Betriebssystemen wurden sie hier
nicht ausgefuehrt. Weder Kompilieren noch Testlauf starten einen
vollstaendigen Massendownload.

Nach dem Download folgt der [Indexaufbau](oparl-index.md);
die [Suche](oparl-search.md) arbeitet danach ohne RIS-Verbindung.
