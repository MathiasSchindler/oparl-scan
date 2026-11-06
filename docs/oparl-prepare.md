# oparl-prepare

[Projektuebersicht und Build](../README.md) |
[oparl-scan](oparl-scan.md) |
[oparl-index](oparl-index.md) |
[oparl-search](oparl-search.md)

`oparl-prepare` erschliesst archivierte PDFs vor dem Indexaufbau. Es verwendet
denselben freistehenden C-/newos-Extraktor wie der direkte Indexer: keine libc,
externen PDF-Programme, OCR-Unterprozesse, KI-Dienste oder Netzwerkzugriffe.
Originale und OParl-Metadaten bleiben unveraendert.

## Benutzung

Befehle im Repository-Wurzelverzeichnis ausfuehren:

```sh
# Nach beendetem Download: wiederverwendbare Dokumentdaten erzeugen.
build/macos-aarch64/oparl-prepare --input downloads --output build/prepared

# Metadaten und vorbereitete Dokumentdaten zu einem Suchindex verbinden.
build/macos-aarch64/oparl-index --input downloads --prepared build/prepared \
  --output build/search/index.ops

# Auch ein einzelnes PDF laesst sich untersuchen.
build/macos-aarch64/oparl-prepare --input /absoluter/pfad/dokument.pdf \
  --output build/prepare-inspection --layout
```

`--input` akzeptiert ein Archiv mit `archive.json`, ein Elternverzeichnis
solcher Archive oder eine einzelne Datei. Innerhalb von `documents/` entscheidet
nicht die Dateiendung: auch archivierte `.sao`-Dateien werden verarbeitet.
Angefangene `.part`-Downloads bleiben ausgeschlossen. Die Ausgabe ausserhalb der
Originalarchive ablegen; Vorbereitung und Download nicht gleichzeitig ausfuehren.

| Option | Bedeutung und Standard |
| --- | --- |
| `--input PFAD` | Archiv, Elternverzeichnis oder PDF; Standard `downloads` |
| `--output VERZEICHNIS` | Ableitungen; Standard `build/prepared` |
| `--templates DATEI` | Regeldatei; Standard `templates/potsdam.json` |
| `--layout` | Positionen auf allen statt nur auf der ersten Seite speichern |
| `--force` | Artefakte neu erzeugen, auch nach Cachebeschaedigung |
| `--help` | Kommandozeilenhilfe |

Die JSON-Regeldatei ist benoetigte Konfiguration, keine Programmbibliothek.
Bei Aufrufen ausserhalb des Repositorys ihren absoluten Pfad angeben.
Ein leerer `templates`-Array erlaubt reine Textextraktion ohne Formularregeln.

Exitcodes: **0** bedeutet erfolgreiche Extraktion, **3** dokumentierte
PDF-Warnungen oder Extraktionsfehler, **1** einen fatalen Lese-, Konfigurations-,
Integritaets-, Ressourcen- oder Schreibfehler. Ein unbekanntes Formular
bedeutet nicht, dass die PDF-Textextraktion fehlgeschlagen ist.
Die JSON-Ausgabe auf stdout zaehlt Quelldateien, Cachetreffer und Dateien mit
Warnungen/Fehlern. Doppelte Bytes koennen mehrere Quelldateien repraesentieren.

## Wiederverwendung und Integritaet

```text
build/prepared/
  manifest.json
  <pipeline-key>/
    <pdf-sha256>.json
```

Der Pipeline-Schluessel bindet den Quellcode-Fingerabdruck des Extraktors,
seiner eingebundenen Laufzeit-/PDF-Grundlagen, die exakten Templatebytes und
den Layoutmodus. Aenderungen daran oder an den PDF-Bytes erzeugen einen anderen
Cachepfad. Die Quellcodebindung entsteht beim Build; ein neues Template
benoetigt keinen neuen C-Build.

Vorhandene Artefakte werden vor Wiederverwendung gelesen und geprueft, aber
nicht erneut geschrieben. Gleiche PDF-Bytes koennen Extraktion teilen; sie
beweisen weder gleiche OParl-Identitaet noch gleiche Paper-Zugehoerigkeit.
Auch dokumentierte PDF-Fehler sind wiederverwendbare Ergebnisse.

Artefakte und Manifest werden ueber temporaere Dateien und Umbenennung
veroeffentlicht. Das Manifest wird erst nach dem vollstaendigen Eingabelauf
aktualisiert. Abgebrochene Laeufe duerfen bereits fertige, wiederverwendbare
Artefakte hinterlassen. Alte Pipeline-Verzeichnisse werden nicht automatisch
geloescht.

Jede Dokumentdatei hat die Form
`{"format":1,"sha256":"...","data":{...}}`. Die Pruefsumme gilt fuer die exakten
serialisierten Bytes von `data`, einschliesslich aller Leerzeichen.
Der Leser prueft ausserdem Pipeline, Extraktor, Quellhash, Bytezahl,
Seitenfolge und Warnungswerte. Eine beschaedigte Datei wird ausdruecklich
abgewiesen, nicht still durch erneute Extraktion ersetzt.
`--force` repariert solche abgeleiteten Dateien; Originale bleiben unberuehrt.
Die Pruefsumme ist keine Signatur: Der Cache muss wie der Index vor
unautorisierten Schreibzugriffen geschuetzt werden.

`oparl-index --prepared ...` fixiert den Manifest-Schluessel fuer den Lauf.
Fehlende, veraltete oder ungueltige Artefakte stoppen den Indexbau; es gibt
keinen versteckten Rueckfall auf PDF-Parsing. Die Originalbytes werden weiterhin
gelesen und gehasht, um die verwendete Fassung zu verifizieren. Ohne
`--prepared` bleibt der bisherige direkte Indexaufbau verfuegbar.

## Dokumentvertrag

`data` enthaelt:

- `sourceSha256`, `sourceBytes`, `key`, `engine`: Quellfassung und Pipeline.
- `status`: `complete`, `warnings` oder `failed`; dazu `error`, `pageCount`
  und `warningPages`. `complete` beschreibt die Extraktion, nicht die fachliche
  Vollstaendigkeit.
- `matchedHeaders`, `unmatchedHeaders`, `ambiguousHeaders`: Formularerkennung
  auf der ersten Seite; keine ausgedachten Konfidenzprozente.
- `pages`: 1-basierte Seitenfolge mit `text` und `warnings`. Die bisherigen
  Warnungsbits bleiben erhalten: 1 unbekannte Zeichen, 2 nicht unterstuetzter
  Stream, 4 Bildseite mit OCR-Bedarf.
- `fields`: getrennte, belegte Formularbefunde mit Feldname, Wert, Status,
  Template-ID, Regelversion, Seite, Quellenbeschriftung, maximaler Zeilenzahl
  und Originaltext. Mehrzeilige Textwerte werden mit einfachen Leerzeichen
  verbunden; `sourceText` und seine Bytepositionen bleiben unveraendert.

Der normalisierte Seitentext bleibt fuer Suche und Abschnittsidentitaeten
massgeblich. `layout.text` erhaelt dagegen den Text **vor**
Bindestrichnormalisierung; `spans.start/length` sowie
`fields.layoutTextStart/layoutTextLength` sind UTF-8-Bytepositionen in
diesem Layouttext, nicht in `pages.text`.

Die Spannen enthalten Start-/Endpositionen der Textgrundlinie in
PDF-Benutzerkoordinaten, auf Tausendstel Einheiten quantisiert
(`pdf-user-space-milliunits`). Seitenrotation und CropBox werden nicht in
Bildschirmkoordinaten umgerechnet. `fontSize` ist der urspruengliche
Textoperatorwert, ebenfalls mal 1.000, keine transformierte Glyphenhoehe.
Grafiktransformationen und Form-XObject-Matrizen werden beruecksichtigt.
Das sind **keine Glyphenrechtecke oder garantierten Tabellenzellen**.
Ein Zeilenwechsel bleibt auch dann erhalten, wenn der vorherige PDF-Textlauf
bereits mit Leerzeichen endet. Diese Leerzeichen werden nicht nachtraeglich
aus dem Layouttext geloescht, damit Textspannen ihre Bytepositionen behalten.

Standardmaessig wird Layout nur fuer die erste Seite gespeichert.
`--layout` haelt es fuer jede Seite vor, kostet aber deutlich mehr Platz.
Pro Quelldatei gelten 512 MiB Eingabebudget, pro Artefakt 64 MiB und fuer
gesammelte serialisierte Seiten 32 MiB. Eine Seite hat hoechstens 262.144
Textspannen. Budgetueberschreitungen stoppen mit Fehler statt mit
scheinbar vollstaendigen, abgeschnittenen Daten.

## Templates und erste M5-Bausteine

Die mitgelieferten [Potsdamer Regeln](../templates/potsdam.json) unterscheiden
Formularkoepfe mit `Einreicher:` und `Geschaeftsbereich:`. Sie gewinnen:

- `document.date` aus dem beschrifteten Datumsfeld, nicht aus `created`,
  Kennzeichen oder einem beliebigen Datum im Text;
- `originator.label` aus `Einreicher:` als Quellenbezeichnung;
- `department.label` aus `Geschaeftsbereich:` als Zustaendigkeit, ausdruecklich
  nicht automatisch als Urheberschaft oder Einreichung;
- `document.role` bei ausdruecklicher Antwort- oder
  Fristverlaengerungsueberschrift. `answer` beweist keine abschliessende,
  vollstaendige oder sachlich ausreichende Beantwortung.

Eine Regel hat `id`, gemeinsam erforderliche `anchors`, optionale
`excludeAnchors`/`stopAt`-Textanker und `fields`:

```json
{
  "format": 1,
  "templates": [{
    "id": "mein-formular-v1",
    "anchors": ["/SVV/", "Einreicher:", "Datum"],
    "stopAt": ["Anlass des", "Begruendung:"],
    "fields": [{
      "name": "document.date",
      "label": "Datum",
      "type": "date",
      "placement": "right-or-below"
    }]
  }]
}
```

Geprueft wird der erste Kopfbereich bis maximal 2.048 UTF-8-Bytes und vor
dem ersten `stopAt`-Anker. Gross-/Kleinschreibung und Beschriftungen sind
absichtlich exakt. Die passende Regel mit den meisten erforderlichen Ankern
gewinnt; ein Gleichstand bleibt mehrdeutig. `excludeAnchors` verbieten
eine Zuordnung, beispielsweise zur allgemeinen Antwortrolle bei einer
Fristverlaengerungsueberschrift. Fundstellen im spaeteren
Antworttext duerfen dadurch nicht zur Rollenklassifikation werden.

`type` ist `date`, `text` oder `literal`; letzteres uebernimmt einen
konfigurierten `value` fuer eine ausdrueckliche Rollenbeschriftung.
Als Feldnamen sind ausser den oben genannten auch `document.reference`
fuer ein beschriftetes Kennzeichen vorgesehen. Unbekannte Optionen,
unpassende Typen, doppelte Schluessel, Feldnamen oder Template-IDs werden
abgewiesen.

Die aelteren Formulare mit `Drucksache Nr.` und `Erstellungsdatum` haben
eigene Regeln, auch ohne Doppelpunkt hinter dem Erstellungsdatum. Dadurch
wird `Datum der Sitzung` in einer Beratungstabelle nicht mit dem
Dokumentdatum verwechselt. Zusaetzlich zum Erstellungsdatum werden
folgende beschriftete Angaben getrennt gewonnen:

| Feld | Quellenbeschriftung und Bedeutung |
| --- | --- |
| `document.releaseDate` | `Freigabedatum`: Freigabe, nicht Erstellung |
| `submission.receivedDate` | `Eingang Buero der SVV`: Eingang beim genannten Buero, nicht Erstellung der Anfrage |
| `submission.forwardedDate` | `weitergeleitet an das Buero OBM`: verzeichnete Weiterleitung |
| `answer.dueDate` | `Termin der Beantwortung`: verzeichnete Antwortfrist |
| `answer.extendedDueDate` | `Terminverlaengerung`: im Formular verzeichnete verlaengerte Frist |
| `answer.receivedDate` | `Eingang der Beantwortung`: Eingang, nicht Erstellung oder Vollstaendigkeit der Antwort |

Die Beschriftungen in der Regeldatei verwenden die exakten Umlaute des
PDF-Texts. Ein ausgefuelltes Frist- oder Eingangsfeld klassifiziert das
Dokument nicht automatisch als Antwort oder Fristverlaengerungsbitte.
Leere Zellen bleiben `missing`; das Datum aus der naechsten beschrifteten
Zeile darf nicht uebernommen werden.

Werte muessen rechts auf derselben Grundlinie (Toleranz 2 PDF-Einheiten)
oder, bei `placement: "below"`/`"right-or-below"`, 4 bis 22 Einheiten darunter
und hoechstens 4 Einheiten horizontal versetzt beginnen. Standard ist
`"right"`. Datumswerte bleiben auf eine Grundlinie begrenzt.
Textfelder koennen mit `maxLines` zwischen 1 (Standard) und 4 Zeilen
erlauben. Die Potsdamer Akteursregeln erlauben vier. Fortsetzungen muessen
4 bis 22 Einheiten tiefer und hoechstens 4 Einheiten gegenueber dem
Wert- oder Beschriftungsanfang versetzt beginnen. Zu viele Zeilen oder
unerwartete Platzierung verwerfen den gesamten Wert statt nur seinen
ersten Teil auszugeben. Textfelder bleiben auf 256 Quelltextbytes begrenzt.
Ein feldbezogenes `stopAt`-Array setzt zusaetzliche Textgrenzen,
etwa vor der benachbarten Spalte `oeffentlich` im alten Anfrageformular.
Auch die anderen konfigurierten Feldbeschriftungen begrenzen einen Wert.
Schraege Grundlinien, unerwartete Abstaende, mehrere gleiche Beschriftungen
oder PDF-Warnungen ergeben keine scheinbar sicheren Werte.

Datumswerte muessen gueltige Kalenderdaten `DD.MM.YYYY` zwischen 1800 und
2199 sein. `01.01.2000` bleibt als verdaechtiger Platzhalter ausgeschlossen.
Feldstatus sind `rule-supported`, `missing`, `ambiguous`, `invalid`,
`placeholder`, `too-long` oder `unsupported-placement`; nur bei
`rule-supported` ist `value` gesetzt. Es werden keine Personen-IDs geraten.

## Nutzung im Index und Grenzen

Ein vorbereiteter Index behaelt die Rohmetadaten und fuehrt die kompakten
Ableitungen separat als `preparation`, auch ueber CLI, HTTP und MCP.
Er bleibt ohne Cache oder Originalarchiv lesbar. Anerkannte Feldwerte sind
suchbar. Ein belegtes Dokumentdatum kann als `model.dates.document.date`
mit Quellfassung, Seite und Ableitungsregel in Datumsfilter eingehen;
vorhandene Quelldatumsangaben werden nicht still ueberschrieben.
Ein solches Datum ist weiterhin weder Einreichungs- noch Antwortzeitpunkt.
Die weiteren Datumsrollen bleiben getrennte `preparation.fields`-Befunde.
Sie werden nicht in bestehende Such-Datumsrollen wie `submittedAt`
umgedeutet; entsprechende fachliche Filter und Friststatistiken bleiben
einem spaeteren M5-Ausbau vorbehalten.

Dies ist die gemeinsame technische Grundlage fuer M5 und kleinere M6-Fundstellen,
**nicht die vollstaendige Erledigung dieser Meilensteine**. Insbesondere fehlen
Personen-/Fraktionsidentitaeten, Fachsegmentierung,
Tabellen und eine verlaessliche Erkennung von Antworten ohne ausdrueckliche
Antwortueberschrift. Beim realen `26/SVV/0677-002` sind Datum und
Einreicherbezeichnung belegbar, die Antwortrolle bleibt vorsichtig ungeklart.
Ein Dateinamensuffix oder der OParl-Rohtyp wird dafuer nicht umgedeutet.

Die [Vorbereitungstests](../tests/prepare.mjs) pruefen unter anderem
Textparitaet, Layoutfundstellen, Positionsgrenzen, konservative Templates,
Zeilenwechsel trotz abschliessender Leerzeichen, begrenzte mehrzeilige
Akteure, alte Formulare mit getrennten Datumsrollen und leeren Zellen,
OCR-Luecken, Cachewiederverwendung/-invalidierung/-korruption, atomar erhaltene
vorherige Indizes, unveraenderte Originale und Verzeichnisse mit mehr als
1.024 Dateien. Build und Importpruefung umfassen das vierte native Binary.
