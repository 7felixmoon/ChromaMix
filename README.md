# Mixer Colors für OBS

Färbt die Kanäle im OBS-Audiomixer pro Quelle ein: farbige Kopfleiste (statt „Global“-Blau), optional ein farbiger Streifen unter dem Pegel und farbige Quellennamen.

Gebaut und abgestimmt auf **OBS 33.0.3** (Windows).

## Bedienung

- **Rechtsklick auf einen Kanal im Mixer → Farbe**: Vorschlagsfarbe wählen, eigene Farbe oder Farbe entfernen.
- **Tools → Mixer-Farben**: Übersicht aller Audioquellen, Farben ändern, Optionen für Streifen und Namensfarbe.

Die Farben hängen an der Quelle selbst (UUID), Umbenennen ist also kein Problem.
Gespeichert wird in `%APPDATA%\obs-studio\plugin_config\obs-mixer-colors\colors.json`.

## Windows-Build über GitHub (ohne Visual Studio)

1. Auf github.com ein neues Repository anlegen, z. B. `obs-mixer-colors`.
2. Den kompletten Inhalt dieses Ordners hochladen (inklusive des versteckten Ordners `.github`).
3. Im Repository auf **Actions** gehen. Der Build „Push“ startet automatisch (ca. 5–10 Minuten).
4. Wenn er grün ist: den Lauf öffnen und unten unter **Artifacts** die Datei `obs-mixer-colors-1.0.0-windows-x64-…` herunterladen.

## Installation

1. OBS schließen.
2. Das heruntergeladene Artifact entpacken. Darin liegt eine ZIP mit dem Ordner `obs-mixer-colors`.
3. Diesen Ordner nach `C:\ProgramData\obs-studio\plugins\` kopieren (Ordner `plugins` ggf. anlegen).
   Ergebnis: `C:\ProgramData\obs-studio\plugins\obs-mixer-colors\bin\64bit\obs-mixer-colors.dll`
4. OBS starten.

## Hinweis

OBS hat keine offizielle Schnittstelle zum Umfärben des Mixers. Das Plugin klinkt sich direkt in die Oberfläche ein.
Ein späteres OBS-Update, das den Mixer umbaut, kann es daher brechen. Dann wird es angepasst.
