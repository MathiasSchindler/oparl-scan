# Gemeinsame Funktionen: Todo-Liste

## Ziel und Leitplanken

Passgenaue Funktionen aus newos verwenden und gezielt nach
[vendor/newos/src/shared](vendor/newos/src/shared) befoerdern.
Keine neue Abstraktionsebene, keine Wrapper nur zur Vereinheitlichung und
keine generischen Callback- oder Framework-Loesungen.

Die Prioritaeten in der folgenden Reihenfolge bearbeiten. Ein Functionmap-
Treffer ist ein Suchhinweis, kein Nachweis fuer gleiche Semantik oder
tatsaechliche Nutzung durch mehrere Programme.

## Prioritaet 1: Vorhandene Shared-Funktionen in Allris verwenden

- [x] Fuer Allris-Hilfsfunktionen passende vorhandene APIs in
  [newos/src/shared](vendor/newos/src/shared) suchen. Die gepinnte
  Vendor-Revision und den aktuellen newos-Stand unterscheiden.
- [x] Signaturen, Fehlerverhalten, Speicherbesitz, Grenzen, Plattformverhalten
  und gegebenenfalls Performance vergleichen. Nur wirklich passende Funktionen
  direkt verwenden.
- [x] Die eigenen Digest-Hexfunktionen `hex_digest` und `eg_hex_digest` aus
  [embedding/index.c](src/embedding/index.c) und
  [embedding/index_io.c](src/embedding/index_io.c) durch das vorhandene
  `hash_to_hex` aus [hash_util.c](vendor/newos/src/shared/hash_util.c) ersetzen;
  alle Aufrufstellen umstellen und die entfallenen Implementierungen entfernen.
- [x] Vorhandene Puffer-, Zahlenformatierungs-, JSON-Ausgabe-, Pfad- und
  Dateisystemfunktionen auf direkte Wiederverwendung pruefen. Insbesondere
  gepufferte Ausgabe nicht durch langsame byteweise Dateideskriptor-Ausgabe
  ersetzen.
- [x] Pro Umstellung die betroffenen Programme bauen und gezielte Tests
  ausfuehren. Ausgabeformate und Fehlervertraege erhalten.

## Prioritaet 2: Geeignete newos-Tool-Funktionen nach Shared befoerdern

- [x] Fuer noch ungedeckten Allris-Bedarf passende Implementierungen in
  [newos/src/tools](vendor/newos/src/tools) mit der C-Version von Functionmap
  suchen und die Treffer an den konkreten Aufrufstellen pruefen.
- [x] Gepufferte Ausgabe vorzeichenloser Zahlen untersuchen: Allris-Helfer
  wie `number` und `out_uint` mit `git_buffer_append_unsigned`,
  `pgpkey_sqs_write_uint_value` und `rg_write_uint_buffered` vergleichen.
  Zuerst ausschliessen, dass Prioritaet 1 den Bedarf bereits abdeckt.
- [x] Vor einer neuen JSON-Implementierung auch vorhandene lesende
  JSON-Funktionen in newos-Tools auf Eignung pruefen.
- [x] Geeignete Funktionen mit ihren notwendigen, ebenfalls allgemeinen
  Abhaengigkeiten nach Shared verschieben. Tool-spezifische Zustaende,
  Regeln und Fehlermeldungen im jeweiligen Tool lassen.
- [x] Bestehende newos-Tools und Allris direkt auf dieselbe Shared-Funktion
  umstellen; die alten Implementierungen entfernen.
- [x] Falls neue Quelldateien entstehen, die
  [source_manifest.h](vendor/newos/src/compiler/source_manifest.h) aktualisieren.
  Betroffene newos-Tools freestanding bauen und gezielt testen.
- [x] Die Vendor-Revision nach Bereitstellung der upstream Aenderungen
  aktualisieren und Allris damit bauen und testen.

## Prioritaet 3: Mehrfach genutzte Allris-Funktionen nach newos heben

- [x] Erst verbleibende Luecken nach Prioritaet 1 und 2 betrachten.
- [x] Mit Functionmap einschliesslich `--within-tool` Allris-interne
  Duplikate und nahe Treffer erfassen. Fuer jeden Kandidaten anhand von
  Makefile und Aufrufstellen belegen, welche unterschiedlichen Programme
  ihn tatsaechlich verwenden; doppelte Definitionen allein reichen nicht.
- [x] Die verwandten Parser in
  [urspruenglich json.c](https://github.com/MathiasSchindler/oparl-scan/blob/bd74c56e768a76b5070994790d0309beb90b5db3/src/json.c) und
  [urspruenglich embedding/json.c](https://github.com/MathiasSchindler/oparl-scan/blob/bd74c56e768a76b5070994790d0309beb90b5db3/src/embedding/json.c) auf eine gemeinsame, konkrete
  lesende JSON-API pruefen. Unterschiede bei Typen, Unicode, Zahlen,
  Grenzen und Fehlerbehandlung ausdruecklich abgleichen.
- [x] Die strikt gleichen Verzeichnishelfer in
  [embedding/index.c](src/embedding/index.c) und
  [embedding/index_io.c](src/embedding/index_io.c) pruefen. Berechtigungen,
  bestehende Pfade und Symlink-Verhalten vor einer Uebernahme festlegen.
- [x] Die gleichen FNV-1a-Stringhashfunktionen in
  [oparl-index.c](src/oparl-index.c) und [search.c](src/search.c) auf
  allgemeinen Nutzen und bereits vorhandene Alternativen pruefen.
- [x] Nur belegbar mehrfach genutzte, domaenenunabhaengige Funktionen mit
  passgenauen Vertraegen uebernehmen. RIS-Regeln, Suchindex-spezifische
  Strukturen und reine Embedding-Modellinternas nicht pauschal auslagern.
- [x] Shared-Tests ergaenzen, gegebenenfalls das Quellmanifest aktualisieren,
  alle betroffenen Allris-Programme umstellen und Duplikate entfernen.

## Abschluss je Umstellung

- [x] Nachvollziehbar festhalten: Herkunft, Nutzerprogramme, ersetzte
  Implementierungen, gewaehlte API und Testergebnisse.
- [x] Keine externen Laufzeitabhaengigkeiten, keine Standard-C-Bibliothek
  und keine direkten OS-Abhaengigkeiten ausserhalb der Plattformschicht
  einfuehren.
- [ ] Freestanding-Verhalten auf den betroffenen Plattformen pruefen.
  Cross-Syntaxpruefungen nicht als native Laufzeittests ausgeben.
  Nativ ist macOS/aarch64 geprueft. Native Linux-/Windows-Laufzeittests
  bleiben mangels entsprechender Laufzeitumgebung offen.
- [x] Nach gezielten Tests die relevanten Regressionstests ausfuehren;
  Functionmap erneut laufen lassen und entfernte Duplikate kontrollieren.

## Umgesetzt am 2026-11-08

Upstream: [MathiasSchindler/newos#4](https://github.com/MathiasSchindler/newos/pull/4).
Zu Beginn waren Vendor-Pin und aktuelles `origin/main` beide
`72d2bfed00a770c0e2aa0ee95615cbca16ff18f0`.
Der neue Pin ist der veroeffentlichte PR-Commit
`3cd45492dda11f6fff702b035baa95f237579f29`, nicht der bisherige main-Stand.
Die Allris-Aenderungen liegen im Arbeitsbaum; die newos-Aenderungen sind
im PR bereitgestellt, ohne den PR automatisch zu mergen.

### Herkunft, Nutzer und gewaehlte APIs

| Umstellung | Herkunft / entfallene Implementierungen | Tatsaechliche Nutzer | Shared-API |
| --- | --- | --- | --- |
| Digest-Hex | `hex_digest`, `eg_hex_digest` und die Header-Deklaration entfernt; alle Aufrufe umgestellt | `oparl-index`, `oparl-search` | vorhandenes [hash_to_hex](vendor/newos/src/shared/hash_util.c) |
| Zahlen im Speicherpuffer | newos `git_buffer_append_unsigned` befoerdert; Allris `number` in Vorbereitung, PDF-Struktur, Suchmodell und Recherche entfernt; Feldschreiber direkt umgestellt | newos `git`; Allris `oparl-prepare`, `oparl-index`, `oparl-search` | [tool_byte_buffer_append_uint](vendor/newos/src/shared/tool_buffer.c) |
| Zahlen im Ausgabepuffer | newos `rg_write_uint_buffered` befoerdert und entfernt; Allris `out_uint` waehlt direkt Speicher- oder Streampuffer und behaelt die 16-MiB-Grenze | newos `ripgrep`/`rg`; Allris `oparl-search` | [tool_output_buffer_write_uint](vendor/newos/src/shared/tool_io.c) |
| Lesendes JSON | beide lokalen JSON-Parser und ihre Header entfernt; alle Typen, Includes, Aufrufe und Makefile-Objektlisten direkt umgestellt | alle vier Allris-Programme; lange Embedding-Werte in Index und Suche | [ToolJson und tool_json_*](vendor/newos/src/shared/tool_json_parser.h) |
| Gepufferte JSON-Strings | `json_quote`, `eg_json_quote` und der reine Wrapper `search_json_quote` entfernt | alle vier Allris-Programme | [tool_json_append_string](vendor/newos/src/shared/tool_json.c) |
| Verzeichnisduplikat | `ensure_directory` in Embedding-Index entfernt; vorhandenes `eg_ensure_directory` direkt verwendet | nur `oparl-index` | bleibt lokal in [index_io.c](src/embedding/index_io.c), keine neue Shared-API |

Die Nutzeraussagen beruhen auf den [Build-Zielen](Makefile) und Aufrufketten,
nicht allein auf Quelldateinamen oder den verlinkten Objektlisten.

### Vertragsvergleich und bewusste Grenzen

- `hash_to_hex` verwendet fuer die vorhandenen 32-Byte-Digests dieselbe
  kleingeschriebene Hexdarstellung mit Nullterminator im 65-Byte-Ziel.
  Keine Allokation und kein Vendor-Update fuer diesen ersten Schritt.
- Die bereits vorhandenen festen Pufferfunktionen passen nicht direkt zu
  `ToolByteBuffer` beziehungsweise `ToolOutputBuffer`. Die befoerderten
  Zahlenfunktionen verwenden die vorhandene Dezimalformatierung, erhalten
  Pufferbesitz und Fehlerweitergabe und haengen keinen Nullterminator an.
  Die SQS-Laengen-/Spaltenregeln von `pgpkey_sqs_write_uint_value` passen
  nicht zu diesen Puffern und bleiben im Tool.
- Die vorhandene fd-JSON-Ausgabe bleibt unveraendert. Insbesondere wird
  keine gepufferte Allris-Ausgabe durch byteweise fd-Ausgabe ersetzt.
  Der eigene `SearchOutput`-Dispatcher und sein begrenzter JSON-Schreiber
  bleiben wegen des Speicherbudgets lokal; keine Callback-Abstraktion.
- Die Leser in newos `jq` und `wtf` wurden geprueft: Slice-/NUL-Scanning
  und teilweise Anzeige-Normalisierung von Kontrollzeichen sind kein Ersatz
  fuer strikte UTF-8-, Surrogat-, Zahlen- und Gesamtdokumentvalidierung.
  Ihre bestehenden Vertraege werden nicht nebenbei geaendert.
- `ToolJson` leiht den Eingabetext und besitzt nur seine Token-Allokation.
  Byte-Offsets und `next`-Teilbaumgrenzen, sechs Tokenarten, Tiefe 64 und
  hoechstens 2.000.000 Tokens bleiben erhalten. Ein Parsefehler gibt Tokens
  frei. String-Extraktion weist falsche Typen, zu kleine Ziele und dekodierte
  NUL zurueck; vorzeichenlose Zahlen muessen vollstaendig und ueberlauffrei
  sein. RIS-spezifische Regeln bleiben ausserhalb von Shared.
- Der urspruengliche Allris-Vergleich bleibt mit `tool_json_equals`
  allokationsfrei auf 255 dekodierte Bytes begrenzt. Embedding-Vergleiche
  langer IDs/Titel verwenden ausdruecklich `tool_json_equals_alloc`,
  einschliesslich des bisherigen Allokationsfehler-Diagnosetexts.
  Alle von Embedding gesuchten Feldnamen passen in den kurzen Lookup;
  lange Feldwerte werden dadurch nicht begrenzt.
- `tool_json_dup_string` behaelt die Bytebudgets und Speicheruebergabe
  des bisherigen Embedding-Helfers. Der Faktor-sechs-Test fuer die kodierte
  Laenge wurde ohne Multiplikationsueberlauf umgesetzt. Aufrufer behandeln
  Fehler weiterhin und geben erfolgreiche Kopien mit `rt_free` frei.
- Die Verzeichnishelfer verwenden bereits die Shared-Pfad-/Elternfunktion.
  Ihre Aufrufketten laufen nur ueber `search_embedding_build` beziehungsweise
  die integrierten Embedding-Indexer in `oparl-index`. Deshalb keine
  Befoerderung nach newos. Lokal bleiben Blattmodus 0700, Elternmodus 0755,
  unveraenderte vorhandene Modi, bisherige Pfad-/Race-Fehler und
  Unix-Symlink-Pruefungen erhalten. Windows kann mit dem bisherigen Backend
  Reparse-Punkte nicht unterscheiden; keine neue Garantie behauptet.
- `hash_key` und `hash_word` sind zwar identisches 64-Bit-FNV-1a.
  `hash_word` wird aber nur von `intern` in `search_add_record` aufgerufen;
  dessen Nutzer sind `emit` und `extracted_page` in `oparl-index`.
  Auch `hash_key` gehoert ausschliesslich zum Import dieses Programms.
  Das Verlinken von [search.c](src/search.c) in `oparl-search` reicht nicht
  als Mehrfachnutzungsnachweis. Daher bleiben die Hashfunktionen lokal.
  Der newos-`xmlstats`-Hash ist ausserdem anders initialisiert, laengenbasiert
  und bildet null auf eins ab: kein semantisch gleicher Ersatz.
- Die weiteren strikten Treffer sind Schemafeld-Schreiber, erforderliche
  Boolean-Feldpruefungen, der Embedding-Titelkomparator und kurze
  Token-Typpruefungen. Sie rechtfertigen keine neue Wrapper-/Framework-API.
  Nahe Suchmodell-, Provenienz- und Modellkernel-Treffer bleiben ebenfalls
  mit ihren jeweiligen Regeln lokal.
- Der neue Parser ist im newos-Quellmanifest und den expliziten macOS-/
  Windows-Runtime-Listen registriert. Allris bezieht ihn daraus; der
  Extraktor-Fingerabdruck umfasst weiterhin die benoetigten Shared-Quellen
  und Header. Das Verschieben kann vorhandene Vorbereitungscaches korrekt
  invalidieren, ohne ihr Ausgabeformat zu aendern.

### Nachweise

- Je Umstellung native Builds der betroffenen Allris-Programme und
  gezielte JSON-, Embedding-, Such-, Vorbereitungs- und Recherchetests.
- Abschliessend `make -j4 test` gegen den finalen Vendor-Pin bestanden:
  alle vier Builds, JSON/Embedding, Integration, inkrementelle Updates,
  Archiv, Parallelitaet, Suche einschliesslich Web/MCP, Suchmodell,
  Suchsemantik, Recherche und Vorbereitung. Die vier Abhaengigkeitspruefungen
  bestaetigen keine libc-/CRT-Abhaengigkeit.
- newos nativ macOS/aarch64 freestanding gebaut:
  `toolsharedtest`, `git`, `ripgrep`, `rg`; vorhandene Phase-1-Suites
  `tools/system/git` und `tools/text/ripgrep` bestanden.
  Zusaetzliche Abhaengigkeitspruefungen fuer `toolsharedtest`, `git` und
  `ripgrep` bestaetigen ebenfalls keine libc-/CRT-Abhaengigkeit.
- Neue Shared-Tests pruefen Null und den vollen unsigned-64-Bit-Bereich,
  Pufferpraefixe, fehlenden Terminator, gepufferte Ausgabe ohne fd-Zugriff,
  Flushfehler, exaktes JSON-Escaping, Unicode/NUL/Ueberlauf,
  kurze und lange Vergleiche sowie Stringbudgets. Die Grenzen wurden exakt
  getestet: Tiefe 64 akzeptiert, 65 abgelehnt; 2.000.000 Tokens akzeptiert,
  2.000.001 abgelehnt.
- Strikte Freestanding-Syntaxpruefungen mit Clang, `-Wall -Wextra
  -Wpedantic -Werror`, fuer alle 31 Allris-C-Dateien und die befoerderten
  newos-Quellen, Git/ripgrep und Shared-Tests bestanden: Linux und Windows,
  jeweils x86-64 und AArch64. Das sind keine nativen Laufzeittests.
- C-Functionmap erneut mit `--external allris src --tool external/allris
  --within-tool --format json --max-results 0` ausgefuehrt: strikte Paare
  **9 -> 6**, nahe Paare **52 -> 18**, konzeptionelle Paare **63 -> 53**;
  keine Ergebnisabschneidung. Digest-, Verzeichnis- und JSON-Kernduplikate
  sowie ersetzte Zahlenhelfer entfernt; die bewusst ausgeschlossenen
  Kandidaten bleiben sichtbar.

Offen bleibt ausschliesslich die native Linux-/Windows-Laufzeitvalidierung.
