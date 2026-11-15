# MiSTer Moonlight

Client Moonlight per Sunshine sul MiSTer, con uscita sul core H264 (decoder H.264 in FPGA).

## Installazione

1. Copia sulla SD (in `/media/fat`) le cartelle `Scripts` e `MiSTer_moonlight` dello zip,
   e il core H264 in `_Other` (`H264_*.rbf`).
2. Lancia **Scripts → mister_moonlight**: si apre l'elenco dei PC. Scegli *+ Aggiungi PC* e
   scrivi l'indirizzo IP del PC con Sunshine (tastiera a schermo col pad, o una tastiera vera),
   poi un nome. Se in `mister_moonlight.conf` c'è `HOST=...`, quel PC entra nell'elenco da solo.
3. Se in `/media/fat/moonlight-groovy/keys` c'è già un abbinamento, viene riusato; se no il
   menu mostra un PIN da inserire nella pagina web di Sunshine (scheda PIN).

In Sunshine tieni **CAVLC disattivato**: il core decodifica CABAC; uno stream CAVLC viene
riscritto sull'ARM, che regge solo bitrate bassi.

## Comandi

- Menu: croce o frecce; A, Invio o Start conferma; B o Esc indietro (dall'elenco dei PC: esce).
- Elenco dei PC: A si collega, destra apre le opzioni del PC (rinomina, cambia indirizzo,
  rimuovi). Dalle app, B torna all'elenco dei PC.
- Tastiera a schermo (indirizzo e nome del PC): A scrive il tasto scelto, Start o Invio
  conferma, B o Esc annulla; con una tastiera vera si scrive direttamente.
- Tornare al menu durante lo stream: Select+Start+LB+RB, Ctrl+Alt+Shift+Q, oppure
  *Program menu* nell'OSD del MiSTer.
- Tasto Guide: Select+Start, oppure *Send Home to the program* nell'OSD.
- Modalità mouse: Start tenuto da solo 1 s (una breve vibrazione lo conferma). Levetta sinistra
  o croce = puntatore, levetta destra = rotellina, A/B/X = tasti sinistro/destro/centrale.

## Impostazioni

Nel menu: bitrate (automatico = 10 Mbps), audio, chiusura dell'app all'uscita, sincronizzazione. Le scelte
restano in `MiSTer_moonlight/menu.cfg`, con l'elenco dei PC. Lo stream è sempre 640x480 a 60 immagini al secondo
(quanto il core mostra 1:1); da riga di comando `-width`, `-height` e `-fps` restano per le prove.

**Sincronizzazione** (col core di questo repository):
- *genlock*: il core muove il suo PLL video e il raster va alla frequenza del PC (anche 59,94 Hz),
  senza quadri di latenza in più. Si aggancia in 10-20 s dall'avvio e dopo un cambio 240p/480i.
- *genlock +1*: lo stesso, e il core tiene un'immagine decodificata di riserva (un quadro di
  latenza in più). Per i bitrate alti: un'immagine sopra i ~20 KB richiede al core più di un
  quadro di decodifica, e senza riserva viene ripetuta l'immagine precedente.
- *guardia*: un'immagine che arriverebbe a ridosso della presa va alla successiva (fino a un quadro
  in più); *nessuna*: niente.

Se il genlock non lavora (core senza genlock), a fine stream il menu lo dice.

**Risoluzione del PC** (no, 640x480, 1024x768, 1280x960, 1280x720): durante lo stream Sunshine
porta il desktop a quella risoluzione e a fine stream rimette la sua. Con un desktop 16:9 lo stream
640x480 arriva con le bande nere sopra e sotto (le aggiunge Sunshine); con una risoluzione 4:3
l'immagine riempie lo schermo. Lo stream resta 640x480 (1280x720 quindi ha ancora le bande).
Serve una versione recente di Sunshine (una di qualche mese prima di novembre 2026 non cambiava
la risoluzione) con, nelle impostazioni, **Advanced Display Options > Resolution: "Use resolution
provided by the client"**; il monitor del PC (o un display virtuale) deve accettare la risoluzione.
Da riga di comando: `-pcres 1024x768`.
Con `-stats` il log dice ogni 10 s a quanti fps va il PC e scrive `traccia.csv` (solo per le prove).

Sunshine riceve la frequenza dei quadri del core: 59,94 a 480i (con *Refresh Rate* su Auto
il core resta a 59,94), 50 se il core è a 50 Hz. Così nessun frame viene saltato o ripetuto.

## Il core di questo repository

Il core in `core/` (artefatto `H264_core` delle GitHub Actions) ha il quadro largo **640 pixel**
con righe tutte uguali: uno stream 640x480 arriva 1:1, uno 320x240 esattamente x2, senza
ridimensionamenti in orizzontale. Senza genlock va a 60,000 Hz esatti; col genlock segue il PC.

Nell'OSD del core: *Output Mode* 15kHz (480i) o 31kHz (480p); a 15kHz, *15kHz Mode* 240p fa
passare lo stream a 240p (il menu resta a 480i; lo stream 640x480 a righe alterne).
Niente 50 Hz.

Col core originale (H264 v1.1.0) il client funziona lo stesso: Sunshine riceve 59,94 fps e il
menu viene allargato ai 720 pixel del suo quadro.
