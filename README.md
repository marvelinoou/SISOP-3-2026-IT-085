# Laporan Praktikum Sistem Operasi Modul 3

| Nama | Marvelino Davas |
|------|-----------------|
| NRP  | 5027241085      |

---

## Soal 1 - Present Day, Present Time

### Penjelasan

Program ini mengimplementasikan chat server bernama **The Wired** menggunakan TCP Socket. Ada dua program yang dibuat: server (`wired.c`) dan client (`navi.c`), dengan shared definitions di `protocol.h` dan `protocol.c`.

Server menggunakan `select()` untuk I/O multiplexing, artinya satu thread bisa memantau semua koneksi client sekaligus tanpa harus fork atau bikin thread baru per client. Cara kerjanya: `select()` nunggu sampai ada fd yang aktif, kalau ada koneksi baru masuk maka `accept()`, kalau ada pesan dari client yang udah konek maka `recv()` dan proses. Pendekatan ini jauh lebih efisien dibanding fork per client karena tidak ada overhead pembuatan proses baru.

Setiap client yang konek punya state machine sendiri dengan empat tahap:
- `STATE_WAIT_NAME` — client baru masuk, server minta nama
- `STATE_WAIT_PASS` — kalau namanya "The Knights", server minta password
- `STATE_CONNECTED` — client normal yang bisa chat
- `STATE_ADMIN` — client admin yang bisa akses menu RPC

Pengecekan nama dilakukan sebelum client masuk ke state CONNECTED. Kalau nama sudah dipakai client lain, server langsung tolak dan minta nama lain. Ini dijamin lewat fungsi `name_taken()` yang loop semua client aktif. State machine ini penting supaya server tahu harus ngapain dengan setiap pesan yang masuk — apakah diperlakukan sebagai nama, password, chat, atau pilihan menu admin.

Setiap pesan dari client yang sudah CONNECTED akan di-broadcast ke semua client lain yang juga CONNECTED, kecuali pengirimnya sendiri. Admin tidak ikut menerima broadcast karena statenya berbeda. Broadcast dilakukan lewat fungsi `broadcast()` yang loop semua slot client dan kirim pesan ke yang memenuhi syarat.

Client admin "The Knights" dengan password "protocol7" punya akses ke tiga fungsi RPC lewat menu khusus:
1. Check Active Entities — lihat siapa aja yang lagi online beserta jumlahnya
2. Check Server Uptime — lihat sudah berapa lama server jalan dalam format jam/menit/detik
3. Execute Emergency Shutdown — matikan server secara paksa dan disconnect semua client yang sedang terhubung

Di sisi client, `navi.c` menggunakan dua pthread yang jalan paralel supaya bisa recv dan send sekaligus tanpa saling blocking. Tanpa thread, kalau program nunggu input keyboard maka tidak bisa terima pesan dari server, dan sebaliknya. Dengan dua thread masalah ini selesai:
- Main thread: baca input keyboard lewat `fgets()`, kirim ke server via `send()`
- recv_thread: terus-menerus `recv()` dari server, langsung print ke layar

Kalau koneksi terputus dari sisi server (misalnya admin trigger shutdown), recv_thread mendeteksi `recv()` return 0 atau negatif, lalu program otomatis exit. Signal `SIGINT` juga dihandle supaya kalau user tekan Ctrl+C, client kirim `/exit` dulu ke server sebelum tutup koneksi.

Semua event dicatat ke `history.log` lewat fungsi `write_log()` di `protocol.c` dengan format `[YYYY-MM-DD HH:MM:SS] [System/Admin/User] [pesan]`. File log dibuka dengan mode append setiap kali ada event, jadi history tidak pernah terhapus selama file-nya masih ada.

### Cara Penggunaan

```bash
# Compile
gcc wired.c protocol.c -o wired -pthread
gcc navi.c protocol.c -o navi -pthread

# Terminal 1 - jalankan server
./wired

# Terminal 2, 3, dst - jalankan client
./navi
```

### Kendala

-

---

## Soal 2 - The Battle of Eterion

### Penjelasan

Program ini mengimplementasikan game RPG battle berbasis terminal bernama **Battle Eterion**. Ada dua program: server (`orion.c`) yang handle semua logika game, dan client (`eternal.c`) yang jadi antarmuka untuk player. Semua shared definitions ada di `arena.h`.

Bedanya sama soal 1, komunikasi di sini bukan lewat TCP socket tapi lewat **IPC** — artinya hanya bisa dipakai di mesin yang sama (lokal). Ada tiga IPC yang dipakai sekaligus:

| IPC | Key | Fungsi |
|-----|-----|--------|
| Shared Memory | 0x00001234 | Menyimpan seluruh state game (data player, battle aktif, matchmaking queue) |
| Message Queue | 0x00005678 | Komunikasi request-response antara eternal dan orion |
| Semaphore | 0x00009012 | Proteksi akses shared memory dari race condition |

**Routing pesan** dilakukan lewat `mtype` di message queue. Semua request dari eternal ke orion pakai `mtype = 1`. Orion balas ke eternal dengan `mtype = PID eternal`, jadi setiap eternal hanya ambil pesan yang memang ditujukan untuknya. Ini penting supaya balasan dari orion tidak keambil oleh eternal yang salah ketika banyak client konek sekaligus.

**Register dan Login** — data player disimpan secara persistent ke file `players.dat` pakai binary `fwrite`/`fread`. Setiap ada perubahan data (register, battle selesai, beli senjata) orion langsung panggil `save_players()`. Waktu orion restart, data dimuat ulang lewat `load_players()` dan semua runtime field seperti `active`, `in_battle`, dan `in_mm` di-reset ke 0. Username dijamin unik dan satu akun tidak bisa login di dua sesi sekaligus — dicek lewat field `active` di shared memory. Default stats setiap player baru adalah Gold 150, Level 1, XP 0.

**Matchmaking** — dijalankan oleh `mm_thread` yang berjalan di background dan cek antrian setiap detik. Kalau ada dua atau lebih player di antrian, dua yang pertama langsung dipertemukan dan dikeluarkan dari antrian. Kalau seorang player sudah nunggu 35 detik tanpa ketemu lawan, dia otomatis dipertemukan dengan bot. Bot dikendalikan oleh `bot_thread` tersendiri yang menyerang secara otomatis dengan interval acak antara 1 sampai 3 detik. Nama dan damage bot di-random tiap battle.

**Battle** — berjalan real-time dan asinkron, bukan turn-based. Kedua pihak bisa menyerang kapan saja tanpa harus menunggu giliran. Player tekan `a` untuk normal attack dan `u` untuk ultimate (hanya bisa kalau punya senjata). Ada cooldown 1 detik antar serangan yang dicek lewat timestamp serangan terakhir. Terminal di-set ke raw mode selama battle supaya input `a` dan `u` langsung terbaca tanpa perlu tekan Enter. Setelah battle selesai terminal di-restore ke normal. Di sisi client ada `battle_recv` thread yang terus dengerin update dari orion dan redraw layar battle secara real-time.

Formula stat yang digunakan:

| Stat | Formula |
|------|---------|
| Damage | `BASE_DAMAGE + (total XP / 50) + bonus weapon` |
| Health | `BASE_HEALTH + (total XP / 10)` |
| Ultimate | `Total Damage * 3` |
| Level | Naik setiap kelipatan 100 XP |

**Armory** — ada 5 senjata dengan harga dan bonus damage yang berbeda, mulai dari Wood Sword (100G, +5 dmg) sampai God Slayer (5000G, +150 dmg). Orion otomatis pakai senjata dengan damage tertinggi yang dimiliki player. Kalau player punya senjata, ultimate bisa digunakan saat battle.

**Match History** — setiap battle yang selesai dicatat di array `history` dalam struct `Player`, disimpan ke `players.dat`, dan bisa dilihat lewat menu History di eternal. History menampilkan waktu, nama lawan, hasil (WIN/LOSS), dan XP yang didapat.

Semua akses ke shared memory dilindungi oleh semaphore lewat `sem_lock()` dan `sem_unlock()` untuk mencegah race condition ketika banyak eternal konek sekaligus dan mengakses data player yang sama di waktu yang bersamaan.

### Cara Penggunaan

```bash
# Compile semua file
make

# Terminal 1 - jalankan server
./orion

# Terminal 2, 3, dst - jalankan client
./eternal

# Setelah selesai, bersihkan IPC
make clear_ipc
```

### Kendala

-