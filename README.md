# OParl-Scan

OParl-Scan soll oeffentliche Ratsinformationen ueber OParl-Schnittstellen
kommunenuebergreifend archivieren und fuer Auswertungen erschliessen.
Potsdam ist der erste unterstuetzte Endpunkt und der Ausgangspunkt der
Datenanalyse:

<https://www.potsdam.sitzung-online.de/oparl/system>

Das aktuelle Werkzeug ist ein freistehendes C-Downloadprogramm. Es archiviert
das System, alle veroeffentlichten Koerperschaften und deren Sammlungen von
Organisationen, Personen, Sitzungen, Mitgliedschaften, Orten, Wahlperioden,
Tagesordnungspunkten, Drucksachen, Beratungen und Dateien. Der Download der
eigentlichen Dokumentdateien kann abgeschaltet werden. Die beiden
Koerperschaften werden aus der API ermittelt, nicht fest im Code vorgegeben.

Die erlaubten Produktionshosts und URL-Pfade sowie die Jahrgangsauswahl sind
derzeit auf Potsdam abgestimmt. Weitere Endpunkte brauchen ausdrueckliche
Unterstuetzung; allein eine andere Angabe fuer `--system` schaltet keine
beliebigen Hosts frei. Der Projektname beschreibt das allgemeinere Ziel,
nicht die bereits vollstaendige Unterstuetzung aller OParl-Implementierungen.

## Abhaengigkeiten und Kompilieren

Das ausfuehrbare Programm verwendet **keine Standard-C-Bibliothek, CRT,
libcurl, OpenSSL, externe JSON-Bibliothek, Unterprozesse oder Shellbefehle**.
Es nutzt die gemeinsame Laufzeit, Puffer, Unicode-Verarbeitung, SHA-256,
JSON-Ausgabe, TLS- und X.509-Implementierung sowie die Betriebssystem-Backends
von newos. [src/json.c](src/json.c) ergaenzt den lesenden JSON-Parser:
newos' `src/shared/tool_json.c` schreibt JSON, liest es aber nicht.

`vendor/newos` ist ein auf eine bestimmte Revision festgelegtes Git-Submodul
des oeffentlichen [newos-Repositories](https://github.com/mathiasschindler/newos)
unter CC0. Der Quellcode wird separat bezogen und nicht in die Git-Historie von
OParl-Scan kopiert. Das Submodul legt die genaue Revision fuer den Build fest.

Projekt einschliesslich Abhaengigkeit klonen:

```sh
git clone --recurse-submodules https://github.com/MathiasSchindler/oparl-scan.git
cd oparl-scan
```

Bei einem vorhandenen Klon ohne initialisierte Submodule:

```sh
git submodule update --init --recursive
```

Zum Kompilieren werden Clang/LLVM, GNU make und eine POSIX-Shell mit grep,
sed und mkdir benoetigt. Unter macOS kommen Apples Kommandozeilenwerkzeuge
und SDK hinzu. Windows-Builds brauchen LLVMs lld und llvm-dlltool; Git Bash
oder MSYS2 koennen die Build-Shell bereitstellen. Zur Ausfuehrung des
fertigen Programms sind diese Build-Werkzeuge nicht erforderlich.

```sh
make -j4                                    # nativ unter macOS/ARM64 oder Linux
make -j4 TARGET=linux ARCH=x86_64
make -j4 TARGET=linux ARCH=aarch64
make -j4 TARGET=windows ARCH=x86_64
make -j4 TARGET=windows ARCH=aarch64
```

Die Ausgabe liegt unter `build/<target>-<arch>/oparl-scan`, unter Windows
mit der Endung `.exe`. Das verwendete macOS-Backend ohne Bibliotheksimporte
unterstuetzt Apple Silicon. Der macOS-Build erstellt zunaechst den newos-Linker
als Build-Werkzeug mit Host-Laufzeit. Das fertige Downloadprogramm verwendet
dagegen newos' Programmstart und Mach-O-Linker ohne dylib-Importe.
Linux-Builds sind statische ELF-Dateien ohne dynamischen Loader. Windows
importiert nur die Betriebssystem-DLLs KERNEL32, WS2_32 und BCRYPT,
nicht die Microsoft-CRT.

Build-Ergebnisse in `build/` und lokale Downloads in `downloads/` sind von
Git ausgeschlossen. Das Repository enthaelt Programmcode, nicht die
heruntergeladenen Ratsinformationen.

## Verwendung

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
build/macos-aarch64/oparl-scan --year 2025 --output downloads/2025 \
  --skip-unavailable-files --accept-size-mismatch

# Kleiner Live-Test, absichtlich unvollstaendig: Exitcode 2.
build/macos-aarch64/oparl-scan --output build/live-smoke \
  --page-size 2 --max-pages 1 --max-files 1
```

Unter Linux und Windows den passenden Programmpfad einsetzen.
Auf allen Plattformen Schraegstriche `/` in Pfaden verwenden.
Uebergeordnete Verzeichnisse des Ausgabeordners duerfen entsprechend der
newos-Richtlinie zur Verzeichniserstellung keine symbolischen Links sein.

Standardmaessig erfolgt immer nur eine Anfrage gleichzeitig, mit 500 ms Pause
vor jeder Anfrage und bis zu drei Versuchen bei voruebergehenden Fehlern.
Numerische `Retry-After`-Werte bis 300 Sekunden werden beruecksichtigt;
andere Werte fuehren zu einer ausdruecklichen Fehlermeldung. Weiterleitungen
sind auf die oeffentliche API und `egov.potsdam.de/public/` beschraenkt.
Zugangsdaten oder nichtoeffentliche Bereiche werden nicht verwendet.

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

### Archivstruktur und Wiederaufnahme

```text
downloads/
  archive.json
  missing-resources.jsonl                  # HTTP-404-Fehlstellen pro Lauf
  selection.jsonl                         # nur bei Jahrgangsauswahl
  index/<sha256-anfrage-url>.json          # ungefilterte Suchlaufseiten
  metadata/<sha256-anfrage-url>.json
  documents/<sha256-oparl-datei-id>-<bereinigter-originalname>
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

Zur Wiederaufnahme denselben Befehl erneut ausfuehren. Gueltige
zwischengespeicherte Metadaten werden lokal gelesen. Vorhandene Dokumente
werden anhand ihrer angegebenen Groesse geprueft und uebersprungen.
Unvollstaendige `.part`-Dateien werden neu begonnen, nicht byteweise
fortgesetzt. Der Cache dient der Wiederaufnahme, **nicht einer
inkrementellen Synchronisierung**: Aenderungen am Server und lokale
Beschaedigungen bei unveraenderter Dateigroesse werden nicht erkannt.
`--refresh` ersetzt Metadaten und Dokumente; alternativ fuer eine neue
Erfassung ein neues Verzeichnis verwenden. Alte Dateien werden nicht
automatisch geloescht. Nie zwei Prozesse gleichzeitig dasselbe
Ausgabeverzeichnis verwenden lassen.

Der lokale Cache-Abgleich bleibt einstrangig. Fortschrittsstaende werden
hoechstens zweimal pro Sekunde geschrieben, nicht nach jeder gecachten
Ressource. Der anfaengliche unvollstaendige Status und der Abschlussstatus
werden immer sofort geschrieben. Laufende Zaehler koennen kurz nachlaufen;
die Wiederaufnahme stuetzt sich auf die Dateien, nicht auf den letzten
gespeicherten Zaehlerstand.

Ein lokaler macOS/ARM64-Vergleich mit 31.183 gesichteten Drucksachen,
1.833 Ausgangsdrucksachen und 1.832 gecachten Dokumenten verbesserte sich
bei warmem Cache von etwa 3,10 auf 0,86 Sekunden. Gemessen wurde isoliert
der anfaengliche Drucksachenabgleich, nicht der gesamte verknuepfte Graph
oder Netzwerkdownloads. Dafuer waren weder zusaetzliche Threads noch
parallele Netzwerkanfragen noetig.

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
waehrend gueltige Cachedateien wiederverwendet werden. Es ist kein
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

Eine Cachedatei mit abweichender Groesse wird in diesem Modus erneut
heruntergeladen, auch wenn ein frueherer Lauf sie bereits akzeptiert
hat. So wird eine moeglicherweise beschaedigte lokale Datei nicht
allein wegen der gelockerten Groessenpruefung als gueltig behandelt.
Andere gueltige Cachedateien werden wiederverwendet; ein globales
`--refresh` ist nicht noetig. Der Abweichungsbericht wird bei jedem
Lauf neu erstellt und folgt demselben `.part`-/Umbenennungsverfahren
wie der Bericht ueber nicht verfuegbare Dateien.

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

Node.js wird nur fuer Tests und die Pruefung der Laufzeitabhaengigkeiten
benoetigt:

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
geprueft, einschliesslich Wiederaufnahme mit einem hoeheren Zeitlimit,
ohne zuvor abgeschlossene Metadaten erneut herunterzuladen.
Hinzu kommen Jahrgangsauswahl, spaetere verknuepfte Vorgaenge und
Dokumente, Graph-Deduplizierung, abweichende Organisationsfelder,
Cache-Wiederverwendung und Archividentitaet.

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

Lokal unter macOS/ARM64 geprueft wurden native Ausfuehrung, lokale
Testdaten, zertifikatsgepruefte Live-Downloads von API-Daten und PDFs
sowie Cache-Wiederverwendung. Linux- und Windows-Builds fuer x86-64
und ARM64 werden plattformuebergreifend kompiliert und auf ihre
Laufzeitimporte geprueft; auf diesen Betriebssystemen wurden sie hier
nicht ausgefuehrt. Weder Kompilieren noch Testlauf starten einen
vollstaendigen Massendownload.

## Lizenz

OParl-Scan steht unter [CC0 1.0 Universal](LICENSE).
Das separat bezogene newos-Submodul steht ebenfalls unter CC0.
