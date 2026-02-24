---
name: btrfs2ext4-hardening-v3
overview: Nueva iteración de endurecimiento y mejoras para btrfs2ext4, centrada en vulnerabilidades restantes no cubiertas por A1–A20, robustez de journaling/rollback y rendimiento en casos grandes, incluyendo la integración explícita de mem_track_init (A18).
todos:
  - id: validate-btrfs-metadata
    content: "Endurecer validación de metadatos Btrfs: sys_chunk_array_size, nodesize y checksums de nodos."
    status: completed
  - id: unify-ext4-block-allocator
    content: Unificar el asignador de bloques Ext4 y sincronizarlo con los bitmaps finales.
    status: completed
  - id: fix-relocator-blocksize
    content: Pasar el block_size real a relocator_execute y al hash de extents.
    status: completed
  - id: harden-journal-replay
    content: Validar offset/length y checksum en el journal de relocaciones antes del replay.
    status: completed
  - id: optimize-inode-table-build
    content: Optimizar la construcción de la tabla de inodos Ext4 para evitar O(N^2).
    status: completed
  - id: integrate-mem-track-init
    content: Integrar mem_track_init en main (A18) y conectar su uso con estructuras grandes opcionales.
    status: completed
  - id: extend-tests-and-fuzzing
    content: Añadir pruebas end-to-end y fuzzing para Btrfs/Ext4 que verifiquen coherencia y no-crash tras los cambios.
    status: completed
isProject: false
---

# Plan de implementación v3 — btrfs2ext4

## Objetivos

- **Seguridad**: eliminar lecturas/escrituras fuera de rango, overflows de tamaño, y solapes silenciosos de bloques ext4.
- **Robustez**: hacer el conversor resiliente a superblocks/nodos Btrfs corruptos y journals inconsistentes.
- **Rendimiento**: reducir puntos O(N²) restantes para que escale bien con millones de inodos.
- **Observabilidad**: facilitar el diagnóstico con mejores logs y pruebas automatizadas.

---

## Bloque S — Validación estricta de metadatos Btrfs

- **S1 — Acotar `sys_chunk_array_size` del superblock**
  - **Archivos**: `[src/btrfs/chunk_tree.c](src/btrfs/chunk_tree.c)`, `[src/btrfs/superblock.c](src/btrfs/superblock.c)` si aplica.
  - **Acciones**:
    - En `chunk_map_init_from_superblock`, antes de usar `sb->sys_chunk_array_size`, validar:
      - `array_size > 0` y `array_size <= BTRFS_SYSTEM_CHUNK_ARRAY_SIZE` (tamaño del campo en el superblock).
      - En caso contrario, log crítico y abortar conversión.
    - Añadir un test unitario/fuzz que inyecte un superblock con `sys_chunk_array_size` mayor que el buffer y verifique que el proceso sale con error controlado, sin crash.
- **S2 — Rango seguro para `nodesize` Btrfs**
  - **Archivos**: `[src/btrfs/superblock.c](src/btrfs/superblock.c)`, `[src/btrfs/btree.c](src/btrfs/btree.c)`, `[src/btrfs/chunk_tree.c](src/btrfs/chunk_tree.c)`.
  - **Acciones**:
    - En `btrfs_read_superblock`, validar `nodesize`:
      - Debe ser múltiplo de `sectorsize`.
      - Debe estar en un rango razonable (p.ej. 4 KiB–64 KiB o los tamaños soportados por Btrfs que quieras admitir).
    - Rechazar el superblock (mensaje claro) si `nodesize` es 0, menor que `sectorsize` o excesivo (que pueda causar `malloc` de cientos de MiB).
    - Añadir pruebas con imágenes btrfs sintéticas con `nodesize` inválido para confirmar el comportamiento.
- **S3 — Verificación opcional de checksum en nodos de B-tree Btrfs**
  - **Archivos**: `[src/btrfs/btree.c](src/btrfs/btree.c)`, `[src/btrfs/chunk_tree.c](src/btrfs/chunk_tree.c)`, `[include/btrfs/btrfs_structures.h](include/btrfs/btrfs_structures.h)`.
  - **Acciones**:
    - Reutilizar `crc32c` para recalcular el checksum de `struct btrfs_header` en cada nodo antes de procesarlo.
    - Si el checksum no coincide, abortar la conversión con un error explícito de “árbol Btrfs corrupto, ejecute `btrfs check` primero”.
    - Añadir una opción futura (flag CLI) para relajar esta verificación si se considera demasiado estricta, pero por defecto dejarla activada.

---

## Bloque A — Coherencia global de asignación Ext4 (bloques y bitmaps)

- **A1 — Unificar el asignador de bloques Ext4 (crítico)**
  - **Archivos**: `[src/ext4/extent_writer.c](src/ext4/extent_writer.c)`, `[src/ext4/inode_writer.c](src/ext4/inode_writer.c)`, `[src/ext4/dir_writer.c](src/ext4/dir_writer.c)`, `[src/ext4/journal_writer.c](src/ext4/journal_writer.c)`, `[src/main.c](src/main.c)`, `[include/ext4/ext4_writer.h](include/ext4/ext4_writer.h)`.
  - **Acciones**:
    - Diseñar un único `struct ext4_block_allocator` **global** por conversión:
      - Inicializarlo en `btrfs2ext4_convert`, justo después de `ext4_plan_layout`.
      - Pasar un puntero a este allocator a todas las funciones que reserven bloques (`ext4_write_inode_table`, `ext4_write_directories`, `ext4_write_journal`, y cualquier otra futura).
    - Eliminar los allocators locales de cada módulo y adaptar firmas/APIs en los headers.
    - Garantizar que cada llamada a `ext4_alloc_block` marca el bloque tanto en el bitmap interno del allocator como en una estructura global coherente que se vuelque luego a los bitmaps on‑disk.
- **A2 — Reordenar o rediseñar `ext4_write_bitmaps`**
  - **Archivos**: `[src/ext4/bitmap_writer.c](src/ext4/bitmap_writer.c)`, `[src/main.c](src/main.c)`.
  - **Acciones**:
    - Elegir una de estas dos estrategias (documentar la decisión en comentarios):
      1. **Bitmaps al final**: mover `ext4_write_bitmaps` a después de:
        - `ext4_write_inode_table`, `ext4_write_directories`, `ext4_write_journal`.
        - Usar el estado final de los extents (`fs_info->inode_table[*].extents`) y del journal para marcar *todos* los bloques ocupados.
      2. **Bitmaps in‑memory vivos**: mantener un bitmap de bloques usados en memoria que se actualice en tiempo real desde el allocator global y desde escrituras especiales (journal, directorios); `ext4_write_bitmaps` simplemente lo vuelca a disco.
    - Asegurar que los bloques que contienen:
      - datos de ficheros (incluyendo descomprimidos),
      - datos de directorios y árboles HTree,
      - journal,
      están todos marcados como usados en el block bitmap.
    - Añadir pruebas que monten la imagen resultante y pasen `e2fsck -fn` sin errores de tipo “block claimed by inode but not marked in bitmap”.
- **A3 — Recalcular contadores libres a partir de bitmaps finales (refinar A10/A11)**
  - **Archivos**: `[src/ext4/bitmap_writer.c](src/ext4/bitmap_writer.c)`, `[src/ext4/gdt_writer.c](src/ext4/gdt_writer.c)`, `[src/ext4/superblock_writer.c](src/ext4/superblock_writer.c)`.
  - **Acciones**:
    - Confirmar que `ext4_update_free_counts` se ejecuta **después** de escribir los bitmaps definitivos.
    - Verificar que:
      - `bg_free_blocks_count_`* en todos los `ext4_group_desc` reflejan el número real de bits a 0 por grupo.
      - `s_free_blocks_count_*` y `s_free_inodes_count` del superblock concuerdan con la suma de los grupos.
    - Añadir un pequeño test que lea de vuelta GDT+superblock y compruebe coherencia de estos contadores.
- **A4 — `relocator_execute` usando `block_size` real**
  - **Archivos**: `[src/relocator.c](src/relocator.c)`, `[include/relocator.h](include/relocator.h)`, `[src/main.c](src/main.c)`.
  - **Acciones**:
    - Extender la API de `relocator_execute` para recibir `block_size` (el mismo usado en `relocator_plan` y correspondiente a `layout.block_size`).
    - Sustituir el literal `uint32_t block_size = 4096;` por el parámetro.
    - Asegurarse de que el cálculo de `blocks_in_entry`, el hash de extents y la actualización de `disk_bytenr` usan siempre ese `block_size`.
    - Añadir un test que convierta con `--block-size 1024` o 2048 y verifique que los datos y extents son consistentes.

---

## Bloque J — Journal de relocaciones y recuperación

- **J1 — Endurecer `journal_replay` frente a datos corruptos**
  - **Archivo**: `[src/journal.c](src/journal.c)`.
  - **Acciones**:
    - Antes de `malloc(entry.length)` y de cualquier I/O:
      - Rechazar entradas con `length == 0`.
      - Limitar `length` a un máximo (p.ej. 16 MiB) por chunk para evitar OOM.
      - Verificar que `entry.src_offset` y `entry.dst_offset` + `length` caben en `dev->size` (reutilizar la lógica de `device_read/device_write`).
    - Si la validación falla, abortar el replay del journal con un mensaje claro e indicar al usuario que use `--rollback` si es necesario.
- **J2 — Verificar checksum del header del journal**
  - **Archivo**: `[src/journal.c](src/journal.c)`.
  - **Acciones**:
    - En `journal_check`, recalcular el checksum del `journal_header` usando `crc32c` y compararlo con `header.checksum`.
    - Si el checksum no coincide, tratar el journal como inválido (p.ej. devolver un código que fuerce a ignorar el journal y basarse en el migration map, o abortar con mensaje).
    - Añadir un test que corrompa algunos bytes del header y confirme que el replay se rechaza.

---

## Bloque R — Rendimiento y estructura de datos

- **R1 — Optimizar construcción de la tabla de inodos Ext4 (O(N) total)**
  - **Archivo**: `[src/ext4/inode_writer.c](src/ext4/inode_writer.c)`.
  - **Acciones**:
    - Introducir una estructura auxiliar para la búsqueda Ext4→Btrfs:
      - P.ej. un array `btrfs_for_ext4[ext4_ino]` rellenado inmediatamente después de todas las llamadas a `inode_map_add`.
      - O un hash ligero `ext4_ino -> btrfs_ino`.
    - Sustituir el bucle lineal actual por un acceso O(1) a esa estructura en `ext4_write_inode_table`.
    - Validar en tests de estrés que el tiempo de Pass 3 escale linealmente con el número de inodos.
- **R2 — Confirmar bajo coste del nuevo hash de inodos Btrfs**
  - **Archivos**: `[src/btrfs/fs_tree.c](src/btrfs/fs_tree.c)`, `[include/btrfs/btrfs_reader.h](include/btrfs/btrfs_reader.h)`.
  - **Acciones**:
    - Revisar que el crecimiento de la tabla hash `ino_ht` no crea picos de memoria inaceptables (capacidad 2×count, reasignaciones limitadas).
    - Añadir, si es necesario, un límite superior de capacidad basado en memoria disponible, y un modo degradado donde se desactiva el hash y se vuelve al scan lineal.

---

## Bloque M — Gestión de memoria y A18 (mem_track_init)

- **M1 — Integrar explícitamente `mem_track_init` (A18)**
  - **Archivos**: `[src/main.c](src/main.c)`, `[src/mem_tracker.c](src/mem_tracker.c)`, `[include/mem_tracker.h](include/mem_tracker.h)`.
  - **Acciones**:
    - Llamar a `mem_track_init()` al inicio de `btrfs2ext4_convert`, justo después de construir `mem_cfg` o incluso antes, para inicializar el tracker con `MemAvailable` real.
    - Documentar que `mem_track_exceeded()` será la fuente de verdad para decidir si desactivar estructuras auxiliares (como la tabla hash de extents en el relocator).
    - Revisar que se añaden llamadas simétricas a `mem_track_alloc`/`mem_track_free` al crear y liberar grandes buffers/hash tables (relocator, inode map hash, bloom filter) para que los informes sean veraces.
- **M2 — Revisión de tamaños de `malloc` controlados por disco (pasada final)**
  - **Archivos**: todos los que hagan `malloc/calloc/realloc` con tamaños basados en campos Btrfs (xattrs, symlinks, journal, etc.).
  - **Acciones**:
    - Hacer una pasada puntual asegurando que siempre se cumplen estas invariantes antes de allocar:
      - Tamaños validados contra un máximo global razonable.
      - Validación de productos `count * sizeof(T)` evitando overflows (cuando `count` es mayor de 32 bits).
    - Donde ya hay límites (por ejemplo, PATH_MAX para symlinks), comprobar que los mensajes de error son informativos.

---

## Bloque T — Testing, fuzzing y verificación automática

- **T1 — Suite de pruebas end-to-end de conversión**
  - **Archivos**: `[tests/](tests/)` (añadir nuevos), scripts auxiliares.
  - **Acciones**:
    - Crear scripts (bash o C) que automaticen el flujo:
      - `mkfs.btrfs` sobre un archivo.
      - Población con ficheros grandes, pequeños, comprimidos, directorios profundos, symlinks largos, xattrs.
      - Ejecución de `btrfs2ext4` con varias combinaciones de tamaño de bloque.
      - `e2fsck -fn` sobre la imagen resultante, con aserción de 0 errores.
      - Montaje de la imagen ext4 y comparación recursiva de contenidos con el origen (`diff -r`).
- **T2 — Fuzzing de superblock y árboles Btrfs**
  - **Acciones**:
    - Extender `tests/test_fuzz.c` para inyectar variaciones de:
      - `sys_chunk_array_size` fuera de rango.
      - `nodesize` extremos o no alineados.
      - checksums corruptos en nodos de B-tree.
    - Asegurar que el binario falla con mensajes claros pero sin violaciones de memoria (verificado con ASan/UBSan en entornos donde esas libs estén instaladas).
- **T3 — Verificación de coherencia Ext4 post‑conversión**
  - **Acciones**:
    - Implementar pequeñas utilidades internas (o tests en C) que lean los bitmaps y GDT del ext4 generado y verifiquen:
      - Todo bloque referenciado por inodos está marcado como usado.
      - Los contadores de bloques/inodos libres concuerdan con los bitmaps.

---

## Prioridad sugerida

1. **Críticos inmediatos**: Bloque A1–A4 (solapes de bloques, coherencia de bitmaps, uso correcto de block_size en relocator) y S1–S2 (validación de `sys_chunk_array_size` y `nodesize`).
2. **Robustez del journal/rollback**: Bloque J1–J2.
3. **Rendimiento y estructura de datos**: Bloque R1–R2.
4. **Memoria y A18**: Bloque M1–M2.
5. **Testing y observabilidad**: Bloque T1–T3.

