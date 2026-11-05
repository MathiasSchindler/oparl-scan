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

- Archivierte Objekte aus `metadata/`, einschliesslich eingebetteter
  File-Objekte.
- Lokale Dokumente aus `documents/`, soweit sie sich als unterstuetzte
  PDFs verarbeiten lassen.
- Namen, Drucksachennummern, Typen, aufgeloeste Organisationen,
  Beschreibungen, vorhandene Beratungsrollen und Ergebnisse.
- Pro PDF-Seite der extrahierte Text mit Quellenpfad und Seitenzahl.
- Beziehungen zwischen Drucksachen und Dateien fuer kontextbezogene
  Suchfilter.

Ungefilterte Suchlaufseiten in `index/`, HTTP-Pruefdaten in `cache/`
und `.part`-Dateien sind keine Suchinhalte. Nicht jede beliebige
JSON-Eigenschaft wird zu einem eigenen Suchfeld.

Gleiche OParl-IDs aus verschiedenen Jahrgangsarchiven werden
zusammengefuehrt. Vollstaendige Objektantworten haben Vorrang vor
eingebetteten Kurzformen; bei gleichwertigen Antworten entscheidet
`modified`, ersatzweise die lokale Metadaten-Aenderungszeit.
Als geloescht markierte Objekte werden nicht als Treffer ausgegeben.
Auch die Binaerdateien geloeschter File-Objekte werden nicht als
unzugeordnete Dokumente wieder eingefuehrt.

Nicht zuordenbare lokale Dokumente bleiben unter einer ausdruecklich
lokalen `urn:oparl-search:archive-file:...` auffindbar, nicht unter
erfundenen OParl-URLs. Fuer solche Dateien gibt es keine erfundenen
Originalmetadaten.

`underDirectionOf` bezeichnet eine Zustaendigkeit, nicht verlaesslich
die Urheberschaft. Strukturierte Urheberfelder fehlen im untersuchten
Potsdamer Drucksachenbestand.

## PDF-Textextraktion und ihre Grenzen

Der Indexer verwendet den PDF-Parser und die Flate-Dekompression von
newos sowie eine eigene seitenbezogene Font-/ToUnicode-Auswertung.
Unterstuetzt werden unter anderem:

- Flate-Streams und indirekte Contents-Arrays.
- Die tatsaechliche Seitenreihenfolge aus dem PDF-Seitenbaum.
- WinAnsi und ausgewaehlte Encoding-Differences.
- ToUnicode-bfchar/bfrange einschliesslich UTF-16-Surrogaten.
- Form-XObjects, Fontzustand und Textabstaende.

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

Im zuletzt vermessenen Bestand 2023 bis 2026 wurden 2.966 Seiten mit
Warnungen erfasst, darunter 789 mit OCR-Bedarf. 22 Dateien waren nicht
als unterstuetzte PDF verarbeitbar, drei referenzierte Dateien fehlten.
Zwei Quellarchive waren bereits als unvollstaendig markiert.
Von 6.772 ausgewaehlten Drucksachen ist eine ausdruecklich als geloescht
markiert und daher kein Suchtreffer; die uebrigen 6.771 sowie weitere
Kontextdrucksachen sind enthalten. Diese Zahlen sind eine
Bestandsaufnahme, kein Vollstaendigkeitsnachweis fuer das gesamte RIS.

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
das gemeinsame Indexformat in [src/search.c](../src/search.c).

Die [Such- und Indexertests](../tests/search.mjs) erzeugen temporaere
Archive mit mehr als 1.024 Metadatendateien und synthetischen PDFs.
Sie pruefen unter anderem Schriftabbildungen, Flate-Streams,
Contents-Arrays, Seitenreihenfolge, jahrgangsuebergreifende
Deduplizierung, Loeschmarkierungen, nicht zugeordnete PDFs und
OCR-/Extraktionsluecken. Ein fataler Neuaufbau darf einen vorhandenen
Index nicht ersetzen. Produktivarchive werden dabei nicht beschrieben.
Aufruf und Plattformhinweise stehen unter [Tests](../README.md#tests).
