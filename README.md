# OParl-Scan

OParl-Scan soll oeffentliche Ratsinformationen ueber OParl-Schnittstellen
kommunenuebergreifend archivieren und fuer Auswertungen erschliessen.
Potsdam ist der erste unterstuetzte Endpunkt:

<https://www.potsdam.sitzung-online.de/oparl/system>

Das Projekt besteht aus vier freistehenden C-Programmen mit eigenen
Dokumentationsseiten:

| Programm | Aufgabe | Dokumentation |
| --- | --- | --- |
| **oparl-scan** | OParl-Metadaten und Dokumente herunterladen und aktualisieren | [Benutzung, Optionen, Archivformat und Fehlerbehandlung](docs/oparl-scan.md) |
| **oparl-prepare** | PDF-Texte, Positionen und belegte Formularfelder wiederverwendbar aufbereiten | [Benutzung, Cachevertrag, Templates und Grenzen](docs/oparl-prepare.md) |
| **oparl-index** | Gemeinsamen Metadaten- und PDF-Volltextindex aufbauen | [Benutzung, Importregeln, PDF-Grenzen und Abdeckungsbericht](docs/oparl-index.md) |
| **oparl-search** | Metadaten und Volltexte durchsuchen und Inhalte mit Quellen abrufen | [CLI, Weboberflaeche, HTTP-MCP, JSON und Benchmarks](docs/oparl-search.md) |

`oparl-search` bietet eine CLI sowie unter macOS und Linux einen
lokalen Webdienst mit Suchoberflaeche, JSON-API und lesendem
HTTP-MCP-Endpunkt `/mcp`. Alle verwenden denselben Suchkern.

Die erlaubten Produktionshosts und URL-Pfade sowie die Jahrgangsauswahl
des Downloaders sind derzeit auf Potsdam abgestimmt. Weitere Endpunkte
brauchen ausdrueckliche Unterstuetzung; allein eine andere Angabe fuer
`--system` schaltet keine beliebigen Hosts frei. Der Projektname beschreibt
das allgemeinere Ziel, nicht die bereits vollstaendige Unterstuetzung
aller OParl-Implementierungen.

## Abhaengigkeiten und Kompilieren

Alle vier Programme verwenden **keine Standard-C-Bibliothek, CRT,
libcurl, OpenSSL, externe JSON-Bibliothek, Unterprozesse oder Shellbefehle**.
Sie nutzen die gemeinsame Laufzeit, Puffer, Unicode-Verarbeitung,
SHA-256, JSON-Ausgabe, TLS- und X.509-Implementierung sowie die
Betriebssystem-Backends von newos.
[src/json.c](src/json.c) ergaenzt den lesenden JSON-Parser.
Vorbereitung und direkter Indexaufbau verwenden ausserdem den PDF-Parser und die Flate-Dekompression
von newos sowie eine eigene seitenbezogene Font-/ToUnicode-Auswertung.
Externe PDF-Werkzeuge oder OCR-Unterprozesse werden nicht gestartet.

`vendor/newos` ist ein auf eine bestimmte Revision festgelegtes Git-Submodul
des oeffentlichen [newos-Repositories](https://github.com/mathiasschindler/newos)
unter CC0. Der Quellcode wird separat bezogen und nicht in die Git-Historie
von OParl-Scan kopiert. Das Submodul legt die genaue Revision fuer den Build fest.

```sh
git clone --recurse-submodules https://github.com/MathiasSchindler/oparl-scan.git
cd oparl-scan
```

Bei einem vorhandenen Klon ohne initialisierte Submodule:

```sh
git submodule update --init --recursive
```

Zum Kompilieren werden Git (auch fuer den Extraktor-Fingerabdruck), Clang/LLVM, GNU make und eine POSIX-Shell mit grep,
sed und mkdir benoetigt. Unter macOS kommen Apples Kommandozeilenwerkzeuge
und SDK hinzu. Windows-Builds brauchen LLVMs lld und llvm-dlltool; Git Bash
oder MSYS2 koennen die Build-Shell bereitstellen. Zur Ausfuehrung der
fertigen Programme sind diese Build-Werkzeuge nicht erforderlich.

```sh
make -j4                                    # nativ unter macOS/ARM64 oder Linux
make -j4 TARGET=linux ARCH=x86_64
make -j4 TARGET=linux ARCH=aarch64
make -j4 TARGET=windows ARCH=x86_64
make -j4 TARGET=windows ARCH=aarch64
```

Die Ausgabe liegt unter `build/<target>-<arch>/`: `oparl-scan`, `oparl-prepare`,
`oparl-index` und `oparl-search`, unter Windows jeweils mit `.exe`.
Alle folgenden Beispiele verwenden macOS/ARM64; unter Linux und Windows
den passenden Programmpfad einsetzen. Befehle werden im Wurzelverzeichnis
des Repositorys ausgefuehrt.

Das verwendete macOS-Backend ohne Bibliotheksimporte unterstuetzt
Apple Silicon. Der macOS-Build erstellt zunaechst den newos-Linker als
Build-Werkzeug mit Host-Laufzeit. Die fertigen Programme verwenden
dagegen newos' Programmstart und Mach-O-Linker ohne dylib-Importe.
Linux-Builds sind statische ELF-Dateien ohne dynamischen Loader. Windows
importiert nur die Betriebssystem-DLLs KERNEL32, WS2_32 und BCRYPT,
nicht die Microsoft-CRT.

Build-Ergebnisse in `build/` und lokale Downloads in `downloads/`
sind von Git ausgeschlossen. Das Repository enthaelt Programmcode,
nicht die heruntergeladenen Ratsinformationen.

## Vom RIS zur Suche

```sh
# 1. Einen Referenzjahrgang archivieren; derselbe Aufruf aktualisiert ihn spaeter.
build/macos-aarch64/oparl-scan --year 2024 --output downloads/2024

# 2. Nach Ende der Downloads PDF-Inhalte wiederverwendbar vorbereiten.
build/macos-aarch64/oparl-prepare --input downloads --output build/prepared

# 3. Metadaten und vorbereitete Dokumente gemeinsam indexieren.
build/macos-aarch64/oparl-index --input downloads --prepared build/prepared \
  --output build/search/index.ops

# 4. Im gemeinsamen Index suchen.
build/macos-aarch64/oparl-search --year 2024 'Schule OR wohn*'

# Alternativ: lokaler Webdienst mit dauerhaft geladenem Index.
build/macos-aarch64/oparl-search --serve --port 8080
```

Die Weboberflaeche ist dann unter <http://127.0.0.1:8080/> erreichbar.
Mit Ctrl+C den Dienst beenden. API und Betriebsgrenzen erklaert die
[Webdienst-Dokumentation](docs/oparl-search.md#lokaler-webdienst).

Downloads und Indexaufbau nicht gleichzeitig auf denselben Archiven
ausfuehren. Nach aktualisierten Downloads den Index neu aufbauen;
derzeit ist das ein vollstaendiger Indexneuaufbau, keine inkrementelle
Indexierung. Die teure PDF-Aufbereitung wird bei unveraenderten Bytes und
Regeln jedoch wiederverwendet. Ohne `--prepared` bleibt die direkte
PDF-Extraktion im Indexer moeglich. Die Originalarchive bleiben unveraendert.

Die Potsdamer PDF-Regeln erfassen auch aeltere Anfrageformulare mit
getrennten Erstellungs-, Eingangs- und Fristdaten sowie geometrisch
begrenzte mehrzeilige Einreicherangaben. Diese belegten Quellenfelder sind
noch keine fertigen Bearbeitungszeitstatistiken; Details und Grenzen stehen
bei [oparl-prepare](docs/oparl-prepare.md#templates-und-erste-m5-bausteine).

`--year 2024` meint Drucksachenkennzeichen mit `24/`, nicht das Datum
der Drucksache oder PDF. Verknuepfte spaetere Vorgaenge koennen ebenfalls
enthalten sein. Jahrgaenge und Systeme brauchen getrennte
Archivverzeichnisse; der Indexer kann diese zusammenfuehren.

**Vollstaendigkeit ist nicht garantiert:** Fehlende API-Ressourcen,
verweigerte Dokumente und PDF-/OCR-Luecken werden protokolliert und
als unvollstaendiger Archiv- beziehungsweise Indexstatus weitergegeben.
Die Produktseiten erklaeren Berichte und Exitcodes. Eine leere
Trefferliste beweist daher nicht, dass ein Sachverhalt im RIS fehlt.

## Tests

Node.js wird nur fuer Tests, Importpruefungen und den optionalen
Mehrfach-Benchmarktreiber benoetigt:

```sh
make test
make check-deps
make benchmark  # vorhandener Index des lokalen Bestands, kein Download
make TARGET=linux ARCH=x86_64 check-deps
make TARGET=windows ARCH=aarch64 check-deps
```

Die Tests verwenden temporaere lokale Archive, synthetische PDFs
und lokale HTTP-Testserver. Parser-Tests laufen als freistehendes
newos-Programm. Produktivarchive werden nicht beschrieben; Kompilieren
und Testlauf starten keinen vollstaendigen Massendownload.
Produktbezogene Testdetails stehen bei
[oparl-scan](docs/oparl-scan.md#tests-und-verifikation),
[oparl-prepare](docs/oparl-prepare.md),
[oparl-index](docs/oparl-index.md#implementierung-und-tests) und
[oparl-search](docs/oparl-search.md#tests).

Lokal unter macOS/ARM64 geprueft wurden native Ausfuehrung, lokale
Testdaten, zertifikatsgepruefte Live-Downloads von API-Daten und PDFs
sowie Cache-Wiederverwendung. `oparl-prepare` wurde bisher nur nativ unter
macOS/ARM64 gebaut und geprueft. Fuer die bisherigen drei Programme:
Linux- und Windows-Builds fuer x86-64
und ARM64 werden plattformuebergreifend kompiliert und auf ihre
Laufzeitimporte geprueft; auf diesen Betriebssystemen wurden sie hier
nicht ausgefuehrt.

## Lizenz

OParl-Scan steht unter [CC0 1.0 Universal](LICENSE).
Das separat bezogene newos-Submodul steht ebenfalls unter CC0.
