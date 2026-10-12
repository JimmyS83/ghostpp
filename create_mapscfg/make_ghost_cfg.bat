@echo off
chcp 65001 >nul
REM ============================================================
REM  Priklady spusteni make_ghost_cfg.py
REM  Uprav cesty nize. Aktivni je jen "VYCHOZI SPUSTENI" na konci,
REM  ostatni priklady odkomentuj (smaz REM) podle potreby.
REM  JDIR = adresar s common.j a blizzard.j (pro map_crc a map_sha1)
REM ============================================================

set SCRIPT=%~dp0make_ghost_cfg.py
set MAPS_DIR=c:\Ghost\create_mapscfg\maps
set CFG_DIR=c:\Ghost\create_mapscfg\cfg
set JDIR=c:\Ghost\create_mapscfg\jdir

REM --- 1) Jedna mapa, cfg vznikne vedle mapy ---
REM python "%SCRIPT%" "C:\ghost\maps\mojemapa.w3x" --jdir "%JDIR%"

REM --- 2) Jedna mapa, cfg do jineho adresare ---
REM python "%SCRIPT%" "C:\ghost\maps\mojemapa.w3x" -o "%CFG_DIR%" --jdir "%JDIR%"

REM --- 3) Vsechny mapy v adresari (bez podadresaru), cfg vedle map ---
REM python "%SCRIPT%" "%MAPS_DIR%" --jdir "%JDIR%"

REM --- 4) Vsechny mapy v adresari, cfg do jineho adresare ---
REM python "%SCRIPT%" "%MAPS_DIR%" -o "%CFG_DIR%" --jdir "%JDIR%"

REM --- 5) Vcetne podadresaru ---
REM python "%SCRIPT%" "%MAPS_DIR%" -r -o "%CFG_DIR%" --jdir "%JDIR%"

REM --- 6) Jiny prefix map_path, map_type a matchmaking, prepsat existujici cfg ---
REM python "%SCRIPT%" "%MAPS_DIR%" -o "%CFG_DIR%" --jdir "%JDIR%" --prefix "Maps\Download" --type dota --matchmaking dota_elo --overwrite

REM --- 7) Vsechno dohromady ---
REM python "%SCRIPT%" "%MAPS_DIR%" -r -o "%CFG_DIR%" --jdir "%JDIR%" --prefix "Maps\Download" --type dota --matchmaking dota_elo --overwrite

REM ===================== VYCHOZI SPUSTENI =====================
python "%SCRIPT%" "%MAPS_DIR%" -o "%CFG_DIR%" --jdir "%JDIR%"

pause
