# Laporan Praktikum Sistem Operasi Modul 3

| Nama | Marvelino Davas |
|------|-----------------|
| NRP  | 5027241085      |
| Kelas | IT             |

---

## Soal 1 - Present Day, Present Time

### Penjelasan
Program ini mengimplementasikan chat server bernama **The Wired** menggunakan TCP Socket. Server (`wired.c`) mampu menangani banyak client secara bersamaan menggunakan `select()` untuk I/O multiplexing tanpa fork. Client (`navi.c`) menggunakan dua pthread agar bisa menerima dan mengirim pesan secara asinkron.

Server menggunakan state machine per client dengan empat tahap:
- `STATE_WAIT_NAME` — client baru, menunggu input nama
- `STATE_WAIT_PASS` — nama adalah "The Knights", menunggu password admin
- `STATE_CONNECTED` — client normal, bisa mengirim dan menerima pesan broadcast
- `STATE_ADMIN` — client admin, mengakses menu RPC

Setiap pesan dari satu client akan di-broadcast ke semua client lain yang aktif. Nama dijamin unik — jika nama sudah dipakai, client diminta memilih nama lain. Client admin "The Knights" dapat mengakses tiga fungsi RPC: melihat daftar user aktif, melihat uptime server, dan melakukan emergency shutdown. Semua event dicatat ke `history.log` dengan format `[YYYY-MM-DD HH:MM:SS] [System/Admin/User] [pesan]`.

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

### Output

Terlampir

### Kendala


---

## Soal 2 - The Battle of Eterion

### Penjelasan
Program ini mengimplementasikan game RPG battle berbasis terminal bernama **Battle Eterion**. Server (`orion.c`) menangani seluruh logika game, sementara client (`eternal.c`) menyediakan antarmuka pengguna. Komunikasi antara keduanya menggunakan IPC yaitu kombinasi Message Queue, Shared Memory, dan Semaphore.

**IPC yang digunakan:**

| IPC | Key | Fungsi |
|-----|-----|--------|
| Shared Memory | 0x00001234 | Menyimpan seluruh state game (data player, battle aktif, matchmaking queue) |
| Message Queue | 0x00005678 | Komunikasi request-response antara eternal dan orion |
| Semaphore | 0x00009012 | Proteksi akses shared memory dari race condition |

Setiap request dari eternal ke orion menggunakan `mtype = 1`. Orion membalas dengan `mtype = PID eternal` sehingga setiap eternal hanya menerima balasan miliknya sendiri.

Data player disimpan secara persistent ke `players.dat` menggunakan binary `fwrite`/`fread`. Username dijamin unik dan satu akun tidak dapat login di dua sesi sekaligus.

Matchmaking dijalankan oleh thread terpisah (`mm_thread`) yang memantau antrian setiap detik. Jika ada dua player di antrian, langsung dipertemukan. Jika sudah menunggu 35 detik tanpa lawan, player akan melawan bot yang dikendalikan oleh `bot_thread`.

Battle berjalan real-time dan asinkron. Player menekan `a` untuk menyerang dan `u` untuk ultimate (hanya jika memiliki senjata). Terdapat cooldown 1 detik antar serangan. Formula stat yang digunakan:

| Stat | Formula |
|------|---------|
| Damage | `BASE_DAMAGE + (total XP / 50) + bonus weapon` |
| Health | `BASE_HEALTH + (total XP / 10)` |
| Ultimate | `Total Damage * 3` |
| Level | Naik setiap kelipatan 100 XP |

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

### Output

Terlampir

### Kendala
