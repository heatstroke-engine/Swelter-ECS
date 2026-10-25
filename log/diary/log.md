# DevLog — HeatStroke ECS

Registro de progreso diario del desarrollo del ECS de arquetipos con job system multihilo, dentro del TFG de HeatStroke Engine.

---

## 2026-10-25 (#15)

**Fase:** Fase 2 - ECS de arquetipos (implementación)

**Tiempo invertido:** 4h

### Qué he hecho hoy
- `swelter.hpp` dividido en headers dentro de `code/ecs/include/swelter/` (`config`, `entity`, `registry`, `chunk`, `archetype`, `entity_table`). `swelter.hpp` queda como header paraguas, así que el include para usar el ECS no cambia. Cada header compila solo.
- Aserciones en el constructor del arquetipo: los IDs tienen que estar ordenados y sin repetir (`getColumn<T>` usa `lower_bound`).
- Comentarios Doxygen en todos los headers (en inglés), con `@pre` que corresponden a las aserciones de debug.
- Implementada la #15 (tabla de entidades paginada, D-11):
  - `Entity` de 64 bits: 32 de índice y 32 de generación. Una `Entity` construida por defecto es `NULL_ENTITY` (`{UINT32_MAX, UINT32_MAX}`).
  - `EntityRecord` de 16 B `{generación, ArchetypeId, RowLocation}` y `EntityLocation` `{ArchetypeId, RowLocation}`.
  - `EntityTable` paginada: páginas de `ENTITY_PAGE_SIZE` (1024) registros reservadas según hacen falta; el registro se encuentra con desplazamiento y máscara, sin divisiones.
  - `reserve()`: reutiliza el último índice liberado (LIFO) o da uno nuevo, y lanza `std::length_error` si se acaban los índices.
  - `release()`: sube la generación, mete el índice en la lista libre y lo retira para siempre si la generación llega al máximo.
  - `isAlive`, `place`, `location` e `isPlaced`. `debugSetGeneration` solo en debug, para las pruebas.
- Limpieza de `removeRow`: comprueba si la fila es la última comparando índices en vez de punteros, copia la entidad con `getEntities` (sin `reinterpret_cast`) y sin el `emplace()` que sobraba.
- Pruebas en el sandbox: reutilización LIFO con uno y dos índices, generaciones, entidad nula, índices que cruzan páginas, una lista libre de 1025 enlaces, `isAlive` con copias antiguas del mismo índice, `place`/`location`/`isPlaced`, retirada de índices e integración con un arquetipo (crear 3 chunks de entidades, destruir una de cada tres y comprobar que la tabla apunta a la fila real de cada entidad viva). Comprobado que la prueba de integración falla si no se actualiza la entidad movida.

### Decisiones de diseño
- Headers separados en una subcarpeta `swelter/` y un header paraguas en la raíz, como `entt/entt.hpp`.
- `Entity` como dos campos de 32 bits en vez de un `uint64_t` con máscaras: mismo tamaño y más legible.
- Valor por defecto de `Entity` = la nula, para que `Entity{}` no sea un ID válido (`{0, 0}` es la primera entidad).
- La tabla es una instancia (cada mundo tiene la suya) y no estática como el pool: las pruebas son independientes y no hay estado global.
- Lista libre implícita: un registro libre guarda en `location.row` el índice del siguiente libre, sin memoria extra.
- `ensurePage` crea todas las páginas que falten (`while`) y no solo una, para que siga funcionando si algún día se reservan índices no consecutivos (reserva por lotes en la fase 3, cargar partidas).
- La generación sube siempre al liberar; la retirada se decide con la generación nueva. Así ninguna entidad viva tiene generación `UINT32_MAX` y nunca coincide con la nula.
- "Viva" (el ID es válido) y "colocada" (tiene fila) son preguntas distintas: una entidad reservada está viva pero no colocada.
- La tabla solo gestiona IDs y ubicaciones; quitar filas del arquetipo lo hará el mundo.
- Las referencias a `decisions.md` y a las fases no van en los comentarios del código: son documentación del TFG, no de la librería.

### Problemas / bloqueos
- Me costó entender la lista libre implícita: que `free_head` y el "siguiente" sean índices `uint32` dentro de los propios registros, y que `release` escribe el enlace y `reserve` lo lee.
- Primera versión de `reserve()`: devolvía `void`, no avanzaba la cabeza de la lista libre, incrementaba `next_index` dos veces (escribía un registro y devolvía otro) y no inicializaba la generación del registro.
- `ENTITY_PAGE_SIZE % 2 == 0` solo comprueba que es par; cambiado a `std::has_single_bit`.
- `ensurePage` era `noexcept` pero puede lanzar `std::bad_alloc`.
- La primera versión de `release()` comprobaba la retirada con la generación vieja: un índice llegaba a la generación máxima y, al liberarlo otra vez, no hacía nada y la entidad seguía viva.
- Al pasar `removeRow` a índices comparé `row` con `last_chunk` en vez de con la última fila; lo detectó la prueba de borrar la única fila del último chunk.

### Aprendizajes
- Una lista enlazada no necesita punteros ni nodos aparte: los enlaces pueden ser índices guardados en los huecos libres de la propia estructura.
- La generación resuelve los handles colgantes sin avisar a las copias: basta con cambiar la tabla para que las copias antiguas dejen de coincidir.
- Si una excepción sale de una función `noexcept` el programa termina (`std::terminate`); hay que revisar qué puede lanzar antes de marcarla.
- El orden importa para dejar el objeto consistente si algo lanza: reservar la página antes de incrementar `next_index`.
- Comparar índices en vez de direcciones dice lo que se quiere preguntar y evita razonar sobre punteros a columnas o chunks distintos.
- Una llamada con efectos dentro de una aserción desaparece en release.
- Para comprobar que una prueba sirve, hay que romper el código a propósito y ver que falla.

### Próximos pasos
- PR de la #15.
- Decidir si `typeId` del registro deja de ser `noexcept` (hoy, superar `MAX_COMPONENTS` termina el programa en vez de lanzar).
- Añadir a D-05 las alternativas descartadas (alineación a 4 KB, reserva por lotes y pool por arquetipo).
- Siguiente issue de la fase 2.

---

## 2026-10-25 (#14)

**Fase:** Fase 2 - ECS de arquetipos (implementación)

**Tiempo invertido:** 6h

### Qué he hecho hoy
- Implementada la #14 (arquetipo con layout SoA) en `swelter.hpp`:
  - `computeLayout`: a partir de la lista ordenada de IDs calcula la capacidad del chunk y el desplazamiento de cada columna (`ArchetypeLayout`). La columna de entidades va primero, cada columna empieza alineada a 64 B y la capacidad se reduce hasta que el relleno de alineación cabe en el chunk.
  - `Archetype`: se construye a partir de la lista ordenada de IDs y calcula en el constructor el layout, el bitset y una copia de los `ComponentInfo` de sus componentes, todo `const`. Los descriptores de chunk (`Chunk*` y número de filas) son privados y se leen con `getChunks()`.
  - `addRow`: añade la entidad al final del último chunk, pidiendo un chunk al pool solo cuando no hay ninguno o el último está lleno, y devuelve su ubicación (chunk y fila).
  - `getColumn` (por posición y con plantilla) y `getEntities` (un `std::span` con las entidades del chunk).
  - `removeRow` con la política b: destruye los componentes de la fila, mueve la última fila del último chunk al hueco (`memcpy` para triviales, `move_construct` y `destroy` para el resto), devuelve el chunk al pool si se vacía y devuelve la entidad movida (`std::optional<Entity>`).
  - Destructor: destruye los componentes no triviales de todas las filas vivas y devuelve todos los chunks al pool.
- Límite de `MAX_COMPONENTS` (256) en el registro, que lanza `std::length_error` si se supera. `runTimeAssertion` devuelve `bool` y sigue evaluando la expresión en release.
- `Entity` provisional de 64 bits hasta la #15.
- Pruebas en el sandbox: layout del ejemplo calculado a mano (816 filas, columnas en 6528 y 13056), llenar y desbordar chunks, columna de entidades, columnas alineadas, escritura y lectura, los cuatro casos de `removeRow` con un `std::string` y un componente con contador de instancias que comprueba que `removeRow` no pierde ni duplica objetos y que el destructor los destruye todos y devuelve los chunks al pool.

### Decisiones de diseño
- Mínimo de 16 filas por chunk (`MIN_CHUNK_CAPACITY`): limita el espacio perdido al final del chunk a 1/16 (6,25 %) y una fila a ~1 KB.
- Bitset de tamaño fijo (`std::bitset<MAX_COMPONENTS>`) por sencillez.
- El arquetipo gestiona filas y memoria, no tipos: `addRow` deja los componentes sin construir y quien llama los construye en su sitio.
- Descriptor por chunk (puntero y número de filas) en lugar de un total de entidades: deja sitio para metadatos por chunk (tags activables, `Changed<T>`) y para repartir trabajo por chunks en la fase 3.
- Lo que no cambia (`ids`, `layout`, `bitset`, `cmps_info`) es público y `const`; lo que cambia (los chunks) es privado.
- La ubicación de una entidad usa índice de chunk y no `Chunk*`: se puede validar, ocupa la mitad y es lo que guardará la tabla de entidades (D-11).
- El arquetipo guarda sus propios `ComponentInfo` para no consultar el registro en cada operación (y no depender de su mutex en la fase 3).

### Problemas / bloqueos
- En `computeLayout`: el cursor avanzaba desde el valor sin alinear (las columnas se pisaban), se guardaba el final de cada columna en vez del inicio, la condición del bucle miraba el inicio de la última columna en vez del final y una variable declarada dentro del `do { }` no existe en su `while`.
- `lower_bound` devuelve el primer elemento mayor o igual: sin comprobar la igualdad, `getColumn<T>` devolvía la columna de otro componente.
- Al cambiar `memcpy` por `construct_at`, el `+ fila` quedó sobre el `std::byte*` y avanzaba bytes en vez de entidades.
- La primera versión de `removeRow` trataba la fila como AoS (componentes a continuación de la entidad), tomaba como última fila una posición de más y decrementaba una copia del descriptor.
- El primer destructor usaba el mismo índice para filas y componentes y se colgaba.

### Aprendizajes
- En SoA, el componente `k` de la fila `r` está en `data + offsets[k] + r * size_k`: cada columna se indexa con su propio tamaño desde el inicio del chunk.
- Sumar a un puntero avanza elementos de su tipo; la aritmética en bytes se hace sobre `std::byte*`, no sobre `void*` (extensión de GCC).
- `memcpy` y `std::construct_at` generan el mismo código para un tipo trivial; `memcpy` crea objetos implícitamente solo en tipos trivialmente copiables y su ventaja real es copiar bloques grandes.
- `const std::span<T>` hace constante el span, no los elementos: lo que se quiere es `std::span<const T>`.
- Los miembros se inicializan en el orden en que se declaran, no en el de la lista de inicialización.
- Un componente con contador de instancias es la forma de comprobar que no se pierden ni se duplican objetos.

### Próximos pasos
- PR de la #14.
- #15: tabla de entidades paginada (D-11).

---

## 2026-10-24

**Fase:** Fase 2 - ECS de arquetipos (implementación)

**Tiempo invertido:** 4h

### Qué he hecho hoy
- Implementada la #13 (chunks y pool de chunks) en la rama `feature/13_chunk_pool`, dentro de `swelter.hpp`:
  - Constantes `CHUNK_SIZE` (16 KB) y `COLUMN_ALIGN` (64 B) en el namespace `Swelter`. El concept `Component` usa `COLUMN_ALIGN` en lugar del `64` escrito a mano.
  - `Chunk`: struct `alignas(COLUMN_ALIGN)` con un array de `std::byte` de `CHUNK_SIZE`, sin copia ni movimiento, y `static_assert` de que el tamaño es múltiplo de la alineación y de que `sizeof(Chunk) == CHUNK_SIZE`.
  - `ChunkPool` global: `allocate()`, `deallocate()`, `ownedCount()` y `freeCount()`. Es dueño de todos los chunks (`std::vector<std::unique_ptr<Chunk>>`) y guarda los libres como punteros que solo observan (`std::vector<Chunk*>`), reutilizándolos en orden LIFO antes de reservar memoria nueva.
  - En debug: aserciones de `nullptr`, de doble devolución y de que el chunk pertenece al pool, y relleno con `0xCD` al reservar y `0xDD` al devolver.
- Pruebas en el sandbox: reutilización LIFO, alineación, ningún solapamiento entre cualquier pareja de chunks y contadores que confirman que se reutiliza antes de reservar.

### Decisiones de diseño
- Alineación de 64 B y no de 4 KB: el *4K aliasing* depende de la distancia entre direcciones dentro del chunk, no de dónde empieza; alinear a página solo serviría para localizar una cabecera dentro del chunk (descartado en D-05) y desperdiciaría hasta un 25 % de memoria.
- RAII sin `new` desnudos: un único dueño con `unique_ptr` y punteros crudos solo para observar. `shared_ptr` descartado porque no hay propiedad compartida y añade contadores atómicos.
- Reserva con `std::make_unique_for_overwrite` para no poner a cero 16 KB en cada chunk.
- `frees` reserva capacidad al crecer `owned`, para que `deallocate` no reserve memoria nunca y sea `noexcept`.
- Reserva de chunks de uno en uno: el estado estable es reutilización y la reserva por lotes solo acelera el crecimiento. Se mide en la #20; la interfaz no cambiaría. En la fase 3, cachés de chunks por hilo recargadas por lotes.
- Pool global y no uno por arquetipo: con SoA, chunks contiguos no dejan contiguas las columnas, el prefetcher no cruza páginas de 4 KB y un pool por arquetipo retendría memoria con las migraciones de arquetipo, continuas en un RTS.

### Problemas / bloqueos
- `Chunk(const Chunk&&)` no es el constructor de copia sino uno de movimiento con `const`; la copia quedaba prohibida solo por una regla implícita. Corregido con las cuatro firmas canónicas borradas.
- `sizeof(Chunk)` no se puede comprobar dentro del struct porque ahí el tipo aún es incompleto.
- Los `memset` de depuración se ejecutaban también en release; ahora van dentro de `#ifdef HS_DEBUG_MODE`.
- La prueba de solapamiento dependía del orden en que el heap devuelve las direcciones, y la primera corrección restaba enteros sin signo, que dan la vuelta y hacían que la prueba no pudiera fallar. Corregida con `std::max(a, b) - std::min(a, b)` sobre `std::uintptr_t`.
- El build de release falla en `hsecs_benchmarks` porque su caché conserva los benchmarks en ON; se mira en la #20.

### Aprendizajes
- `new T` hace *default-initialization* (memoria sin tocar) y `new T()` hace *value-initialization*: con un constructor `= default`, pone todo el objeto a cero (`memset` visible en el ensamblador).
- `std::vector` solo exige que sus elementos se puedan mover, así que `std::vector<std::unique_ptr<T>>` funciona; al crecer mueve los punteros, no los objetos, que no cambian de dirección.
- La regla de RAII es que un puntero crudo nunca sea dueño, no que no existan punteros crudos.
- `std::aligned_alloc` no funciona en Windows; el `new` alineado de C++17 sí.
- Comparar con `<` punteros a objetos distintos no está garantizado por el estándar; con `std::uintptr_t` sí. Una resta sin signo solo es segura si el primero es mayor o igual.
- Reservar 16 KB con `new` no es una llamada al sistema: el heap ya pide memoria al sistema en bloques grandes.

### Próximos pasos
- #14: arquetipo con layout SoA (D-04, D-05, D-06).
- Añadir a D-05 las alternativas descartadas: alineación a 4 KB, reserva por lotes y pool por arquetipo.

---

## 2026-10-23

**Fase:** Fase 2 - ECS de arquetipos (implementación)

**Tiempo invertido:** 6h

### Qué he hecho hoy
- Creadas en GitHub las issues de la fase 2: la issue padre #21 y las sub-issues #12 a #20, una por capa (registro de componentes, chunks y pool, arquetipo SoA, tabla de entidades, mundo y grafo, queries, command buffers, sistemas y benchmarks).
- Empezada la implementación de abajo arriba con la #12 (registro de componentes) en la rama `feature/12_component_registry`.
- Eliminado el código del ECS dummy de `code/ecs/include` (`ecs.hpp`, `componentstorage.hpp`, `slotmap.hpp`) y desactivados los benchmarks en CMake hasta que el nuevo ECS los soporte.
- Nuevo archivo `code/ecs/include/swelter.hpp` (namespace `Swelter`):
  - `ComponentInfo`: descripción type-erased de un componente (`size`, `align`, `trivially_copyable`, `is_empty`) con punteros a función `destroy` y `move_construct`.
  - `makeCMPInfo<T>()`: genera el `ComponentInfo` de un tipo con lambdas sin capturas (`std::destroy_at` y `std::construct_at`).
  - Concept `Component`: tipo objeto, no array, alineación ≤ 64 B, constructor de movimiento y destructor `noexcept`.
  - `CmpRegistry`: `id<T>()`, `registerComponents<Ts...>()`, `info(id)`, `info<T>()` y `count()`.
- Pruebas en el sandbox: IDs consecutivos, `const T` y `T` comparten ID, flags de un tag y de `std::string`, y mover un `std::string` largo entre dos buffers usando solo la información del registro.

### Decisiones de diseño
- Registro global (como Unity y EnTT): `id<T>()` es la única fuente de verdad, registra el tipo la primera vez que se pide y el ID es su posición en el vector de `ComponentInfo`.
- `registerComponents<Ts...>()` es opcional: sirve para fijar el orden de los IDs al arrancar y evitar registros concurrentes cuando haya hilos (pendiente un mutex).
- `const` y `&` son formas de referirse a un componente, no componentes distintos: `id<T>()` normaliza con `std::remove_cvref_t` y el concept se comprueba sobre el tipo normalizado.
- El `requires` va en `id<T>()` para que el error salga en la línea de quien llama y no dentro del registro.
- Alineación máxima de 64 B porque las columnas del chunk se alinean a 64 B (D-05).
- `id()` no es `noexcept`: el primer registro hace `emplace_back` y un `bad_alloc` se propaga en lugar de terminar el programa.

### Problemas / bloqueos
- `template<typename Component>` no aplica el concept: declara un parámetro de plantilla llamado `Component` que oculta el concept. `id<int[4]>()` fallaba dentro de `std::construct_at` con un error ilegible.
- Comprobar el concept sobre el tipo sin normalizar rechazaba usos legítimos: `T&` no es un tipo objeto y `const std::string` no es *nothrow move constructible* (moverlo usa la copia).
- Con `void`, el concept daba un error de `alignof` porque `is_object_v` no iba la primera.

### Aprendizajes
- Un concept restringe solo si va en lugar de `typename` (`template<Component T>`) o en una cláusula `requires`, que va detrás de `template<...>` o al final de la declaración, después de `noexcept`.
- Las condiciones de un concept unidas con `&&` se evalúan de izquierda a derecha y se paran en la primera que falla: el orden importa para los mensajes de error.
- Las lambdas sin capturas se convierten a punteros a función, lo que permite el type erasure sin `std::function` ni memoria dinámica.
- Un `static` local dentro de una plantilla de función da un valor único por tipo, inicializado la primera vez que se llama.

### Próximos pasos
- #13: chunks de 16 KB y pool global de chunks (D-05).

---

## 2026-10-21

**Fase:** Fase 2 - ECS de arquetipos (diseño)

**Tiempo invertido:** 2h

### Qué he hecho hoy
- Creado `log/design/decisions.md`: registro de decisiones de diseño con contexto, opciones, decisión y consecuencias, para justificarlas en la memoria.
- Las decisiones del ECS dummy pasan a ser histórico (H-01 a H-05): el ECS de arquetipos se diseña desde cero y no hereda nada.
- Diseñado el núcleo de la fase 2:
  - **D-02:** modelo de arquetipos.
  - **D-03:** registro de componentes con tipos de C++ abiertos e ID asignado en ejecución (nivel B), con `ComponentInfo` type-erased como puerta entreabierta a componentes dinámicos.
  - **D-04:** identidad del arquetipo = lista ordenada de IDs, con un bitset auxiliar para las queries.
  - **D-05:** chunks de 16 KB con layout SoA, columnas alineadas a 64 B, política de borrado b (la última fila del último chunk tapa el hueco), pool global de chunks y cabecera fuera del chunk.
  - **D-06:** los tags forman parte del arquetipo como componentes sin columna.
  - **D-07:** transiciones con un grafo de arquetipos, aristas cacheadas y aristas por bundle.
  - **D-08:** queries con caché de arquetipos y actualización incremental (pull), iteración por chunk y modos de acceso.
  - **D-09:** command buffers explícitos en un buffer lineal de bytes, uno por sistema.
  - **D-11:** IDs de 64 bits (índice + generación) y tabla de ubicaciones paginada con lista libre implícita.

### Decisiones de diseño
- Criterio general: al ser un proyecto académico, en algunos casos se elige la opción más sencilla, siempre que no comprometa la fase 3 (multihilo). Las alternativas más elaboradas quedan como mejoras futuras.
- Fuera del TFG: componentes totalmente dinámicos, relaciones al estilo flecs y *shared components*.
- Mejoras futuras si da tiempo: tags y componentes activables, transiciones por lotes e índice inverso de componente a arquetipos.

### Problemas / bloqueos
- Ninguno.

### Aprendizajes
- Cómo resuelven lo mismo flecs, Bevy, Unity DOTS y EnTT: identificación de arquetipos, grafo de transiciones, queries cacheadas y command buffers.
- En un ECS de arquetipos el almacenamiento es type-erased sea cual sea el registro de componentes, porque las combinaciones se crean en ejecución.
- SoA frente a AoS y AoSoA, *false sharing* y *4K aliasing*.
- Las queries que guardan arquetipos en lugar de entidades no se tocan en los cambios estructurales: eso elimina el coste cuadrático de las cachés del dummy.
- Para un RTS con lockstep, la aplicación de los command buffers tiene que ser determinista.

### Próximos pasos
- Cerrar la decisión de sistemas (declaración, dónde vive su query y orden de ejecución).
- Empezar la implementación de abajo arriba: registro de componentes, chunks y pool, arquetipo, tabla de entidades, mundo, queries y command buffers.

---

## 2026-10-21

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 2h

### Qué he hecho hoy
- Integrado EnTT (v4.0.0) con `FetchContent` como dependencia `SYSTEM`, con la opción `HSECS_BUILD_ENTT_BENCHMARKS` y el ejecutable `entt_benchmarks`. Se enlaza igual que `hsecs_benchmarks` (runtime estático), para que la comparación sea justa.
- Reimplementados los 19 benchmarks con EnTT en `enttbenchmarks.cpp`, con los mismos nombres, componentes, tamaños y preparación del mundo. Además, 4 tests solo de EnTT que iteran con views (`*_VIEW`).
- `build_and_run_benchmarks.ps1` acepta `-Ecs hsecs|entt` y guarda `<fecha>_<ecs>_benchmark.json`. Una tarea de VS Code para cada ECS.
- Script `benchmark_to_latex.ps1`: genera un informe LaTeX con los resultados de un ECS.
- Script `compare_benchmarks_to_latex.ps1`: empareja los dos JSON por test y tamaño y genera un informe LaTeX con la comparativa (resumen, gráficos con pgfplots y tablas por test con la relación HS/EnTT). El PDF se compila con Tectonic.

### Decisiones de diseño
- Equivalencias con EnTT: un tag es un componente vacío y un sistema registrado (con caché) es un *owning group*. Registrar un sistema = crear su group.
- Un componente solo puede pertenecer a un group en EnTT: `HealthSystem` posee `CHealth` y solo observa `CTransform`, que pertenece al group de `PhysicsSystem`.
- Los componentes están duplicados en `enttbenchmarks.hpp` porque ese ejecutable no enlaza `hs_ecs`; tienen que coincidir con los de `benchmarks.hpp`.

### Problemas / bloqueos
- En CMake el operador lógico es `OR`, no `||`: Google Benchmark tiene que descargarse si se compila cualquiera de los dos ejecutables.

### Aprendizajes
- Resultado con el tamaño mayor: HeatStroke es más rápido en 7 de 19 tests (destruir entidades con componentes sin sistemas, quitar 2 componentes, registrar un sistema...).
- Lo que tiene cachés es el cuello de botella: `ADDREMOVECMPCACHED` y `DESTROYENTITIESCACHED` con 100K son ×474 y ×365 más lentos que EnTT, por el `std::erase` lineal.
- La iteración es ~×8–×13 más lenta: `GAMESCENARIO` con 1M tarda 7,3 ms frente a 0,71 ms con groups y ~2 ms con views.
- Ambos puntos son justo lo que debe resolver el ECS de arquetipos: datos contiguos por arquetipo y consultas que guardan arquetipos, no entidades.

### Próximos pasos
- Pasar a la Fase 2: ECS de arquetipos single thread.

---

## 2026-10-20

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 3h

### Qué he hecho hoy
- Completada la batería de benchmarks del ECS dummy (1K, 10K, 100K y 1M entidades):
  - Creación: entidades vacías y añadir 1, 3 y 5 componentes.
  - Destrucción: entidades vacías y con 1, 3 y 5 componentes.
  - Quitar 1 y 2 componentes.
  - Acceso con `getComponent`.
  - Iteración con 1 y 2 componentes, y con solo la mitad de las entidades cumpliendo la firma (`HALF`).
  - Añadir y quitar un componente sin cambios en las cachés y con la entidad entrando y saliendo de una caché (`ADDREMOVECMP` / `ADDREMOVECMPCACHED`).
  - Destruir entidades que están en dos cachés (`DESTROYENTITIESCACHED`).
  - Registro de un sistema con N entidades ya creadas (`REGISTERSYSTEM`).
  - Escenario de juego (`GAMESCENARIO`): un frame con `PhysicsSystem`, `HealthSystem` y `RenderSystem` sobre un mundo heterogéneo (50 % unidades, 30 % decorado, 20 % proyectiles).
- Añadidos `CHealth`, `CGeneric<N>` y `HealthSystem` a `benchmarks.hpp`.
- Cada test tiene un comentario que explica qué mide y cómo se prepara el mundo.

### Decisiones de diseño
- Si lo que se mide cambia la estructura del mundo (crear, borrar, añadir o quitar), el mundo se reconstruye en cada iteración en pausa, con un `std::optional<GameEngine>` y `emplace()`. Si no la cambia (iterar, acceder, añadir y luego quitar), se construye una sola vez fuera del bucle.
- Los tests con cachés cuadráticos (`ADDREMOVECMPCACHED` y `DESTROYENTITIESCACHED`) no llegan a 1M entidades: cada iteración tardaría minutos.
- En `HALF` el throughput cuenta solo las entidades que recorre el `foreach` (`n/2`).

### Problemas / bloqueos
- Los tests de iteración reconstruían el mundo en cada vuelta: como el `update` es muy rápido, Google Benchmark hacía cientos de miles de iteraciones y cada una pagaba la preparación. Se arregló construyendo el mundo una sola vez.

### Aprendizajes
- **El precacheado es cuadrático en los cambios estructurales:** sacar una entidad de una caché es un `std::erase` lineal. Al multiplicar N por 10 el tiempo se multiplica por 100. Con 100K entidades, destruirlas tarda ~2,2 s frente a ~0,36 ms sin cachés.
- **La iteración no contigua cuesta:** `HALF` recorre la mitad de entidades que `TWOCMPS` pero tarda prácticamente lo mismo, por los saltos en memoria y la cadena de indirecciones del `foreach` (caché → entidad → key → slotmap → dato).
- **Escenario de juego:** con 1M entidades, tres sistemas triviales ya ocupan ~7,6 ms de los 16,6 ms de un frame a 60 FPS.
- Formas de paliar el coste de las cachés: swap-and-pop con índice inverso (un sparse set, como EnTT) o cambios estructurales diferidos con command buffers. En un ECS de arquetipos el problema desaparece por diseño: las consultas guardan arquetipos, no entidades, y borrar es un swap-and-pop en la tabla del arquetipo.
- Los command buffers serán necesarios igualmente con el job system, porque no se puede cambiar la estructura del mundo mientras otro hilo lo recorre.
- Para comparar de forma justa con EnTT hay que medir su versión con views y con groups, y el ECS de arquetipos con 1 hilo y con N hilos, para separar lo que aporta el diseño de lo que aportan los hilos.

### Próximos pasos
- Sacar los resultados de todos los benchmarks.

---

## 2026-10-19

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 2h

### Qué he hecho hoy
- El modo debug lo decide CMake: `target_compile_definitions(hs_utils PUBLIC $<$<CONFIG:Debug>:HS_DEBUG_MODE>)`. Con el preset debug están activos los asserts, los warnings y el log; con release desaparecen. Ya no hay que comentar un `#define` a mano.
- El log pasa a usarse con macros (`logInit()`, `logPrint(msg)`, `logTrace(msg)`), que en release se expanden a nada. Las implementaciones de `Log::print` y `Log::printTrace` pasan al `.cpp`.
- Integrado Google Benchmark (v1.10.0) con `FetchContent`: CMake lo descarga y lo compila con el mismo compilador que el ECS. Se controla con la opción `HSECS_BUILD_BENCHMARKS` y genera el ejecutable `hsecs_benchmarks`.
- Primer benchmark, `BM_CREATEENTITES`: crear N entidades (1K, 10K y 100K) en un manager vacío. La construcción y destrucción del manager quedan fuera de la medida (`PauseTiming`/`ResumeTiming` con un `std::optional<GameEngine>` y `emplace()`). Reporta el throughput con `SetItemsProcessed`.
- Script `build_and_run_benchmarks.ps1` y tarea de VS Code: compila en release, ejecuta los benchmarks con 10 repeticiones (media, mediana, desviación y cv) y guarda los resultados en `benchmarks/results/<fecha>_benchmark.json`.

### Decisiones de diseño
- Los benchmarks se miden siempre en release: en debug los asserts y la falta de optimización falsean los resultados.
- Google Benchmark se integra con `FetchContent` en lugar de instalarlo aparte: versión fijada en el repo, mismo compilador y nadie necesita instalar nada para compilar el proyecto.
- No se mide la creación del manager, porque no ocurre durante el `update` del juego.
- Los resultados se guardan en JSON, que además de los tiempos incluye la máquina (CPU, cachés) y la fecha, para documentarlos en la memoria.

### Problemas / bloqueos
- Lío con git: apliqué un stash sobre una versión desactualizada de `sw/ecs/basic/1.0` y el pull generó conflictos. Se resolvió abortando el merge, dejando la rama igual que `origin` y aplicando el stash en una rama nueva (`feature/10_benchmarks`).
- CMake en Windows usa por defecto el generador de Visual Studio (NMake); hay que indicar Ninja y el g++ de MSYS2.
- Puse los argumentos del ejecutable de benchmarks (`--benchmark_out`...) como flags del compilador; además `\r` dentro de una ruta en CMake se interpreta como retorno de carro y rompía `build.ninja`.

### Aprendizajes
- Funcionamiento de Google Benchmark: el bucle `for (auto _ : state)` lo controla la librería, los parámetros con `->Arg`/`->Range` y `state.range(0)`, `SetItemsProcessed` para el throughput y `DoNotOptimize` para que el compilador no elimine el trabajo medido.
- Lo que hay al final del cuerpo del bucle (como el destructor de un objeto local) también se mide.
- `#include "..."` busca primero en la carpeta del fichero; `#include <...>` solo en las rutas de include.
- Una macro que se expande a nada garantiza que sus argumentos no se evalúan; una función `inline` vacía no.
- Primeros resultados: crear entidades baja de ~360M/s con 1K a ~53M/s con 100K, por los `resize` del `SlotMap` (capacidad inicial 1000) y porque los datos ya no caben en caché.

### Próximos pasos
- Añadir el resto de tests de rendimiento.
- Exportar los resultados estables a LaTeX para convertirlos en PDF.

---

## 2026-10-18

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 4h

### Qué he hecho hoy
- Las entidades pasan de `std::vector` a `SlotMap<Entity>`. Toda la API pública del manager trabaja con `Key` en lugar de `Entity&`: `createEntity()` devuelve la key, y `removeEntity`, `addComponent/s`, `removeComponent/s`, `addTag/s`, `removeTag/s` y `getComponent` la reciben. Comprueban con `debugAssertion` que la key es válida, y `removeEntity` y `SlotMap::erase` avisan con un `warning` si reciben una key inválida.
- `Key` tiene `operator== = default`, necesario para buscarla y borrarla de las cachés con `std::erase`.
- Terminado `foreach<SYS>(func)`: recorre las keys de la caché del sistema y llama a `func(Key, CMPs&...)` con los componentes de la firma en el orden de `SCMPs`. Desempaqueta la `Typelist` de la firma en `foreach_impl` y accede a los componentes con un `getComponent` privado que no vuelve a buscar la entidad. Comprueba con un assert que la caché corresponde a la firma del sistema.
- Las cachés se mantienen actualizadas en todos los cambios estructurales:
  - Cada operación tiene una versión privada `_impl` (`addComponent_impl`, `addTag_impl`, `removeComponent_impl`, `removeTag_impl`) que modifica la entidad sin tocar las cachés.
  - Las públicas guardan las máscaras previas de la entidad, llaman a las `_impl` y al final llaman una vez a `updateCacheFor(e, old_cmp_mask, old_tag_mask)`. Las versiones plurales también hacen una sola actualización.
  - `updateCacheFor` compara, para cada caché, si la entidad la cumplía con las máscaras viejas y si la cumple con las nuevas. Así sabe si añadirla, quitarla o no hacer nada, sin buscarla en la caché.
  - `removeEntity` saca la entidad de las cachés que cumple **antes** de quitarle los componentes, y luego los borra con las `_impl`.
- Documentado `ecs.hpp` con comentarios Doxygen.

### Decisiones de diseño
- La API pública recibe keys, no referencias a entidades: una referencia se invalida si el slotmap crece o mueve datos, y la key detecta además si la entidad ya no existe.
- Las versiones `_impl` son privadas en lugar de añadir un parámetro `bool` a las públicas: así el usuario no puede dejar las cachés desincronizadas.
- La pertenencia a una caché solo depende de las máscaras finales, así que basta con una actualización al final de cada operación, aunque cambie varios componentes o tags.

### Problemas / bloqueos
- En `foreach`, `(getComponent<CMPs>(e), ...)` es un fold sobre el operador coma: evalúa todas las llamadas y solo pasa el último componente. Para pasarlos todos como argumentos hay que expandir el pack: `getComponent<CMPs>(e)...`.
- En `removeEntity`, comprobar las cachés después de borrar los componentes no funcionaba: la máscara ya estaba vacía y la entidad se quedaba en las cachés con una key inválida.

### Aprendizajes
- `operator== = default` hace que el compilador genere la comparación miembro a miembro; sin declararlo, una struct no tiene `==`.
- Diferencia entre fold expression (`(expr, ...)`) y expansión de pack en una lista de argumentos (`f(expr...)`).
- Las funciones miembro de una plantilla de clase no se instancian hasta que se usan, así que un error dentro de una función que nadie llama puede pasar desapercibido al compilar.

### Próximos pasos
- Emplear Google Benchmark para medir el rendimiento y poder comparar luego este ECS con el futuro ECS de arquetipos/jobs y con EnTT.

---

## 2026-10-18

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 4h

### Qué he hecho hoy
- Reorganizado el proyecto en módulos: `code/utils` (log y asserts), `code/mp` (metaprogramación) y `code/ecs` (el ECS). En CMake hay un target por módulo (`hs_utils` estático, `hs_mp` y `hs_ecs` de solo cabeceras), enlazados como `ecs → mp → utils`, de modo que cada módulo solo ve los includes de los módulos de los que depende.
- Añadido `registerSystem<SYS>()`: los sistemas declaran su firma con `using SCMPs = MP::Typelist<...>` y `using STAGs = MP::Typelist<...>`, y el manager calcula las máscaras en compilación. Con `compileTimeAssertion` se comprueba que todos los componentes y tags de la firma existen en el manager (`Typelist::includes`).
- Cachés de sistema (`SystemCache`): guardan la firma y la lista de entidades que la cumplen. Si ya existe una caché con la misma firma (comparando por igualdad), se reutiliza. Si no, se crea y se rellena con las entidades que ya existen.
- Empezado el `foreach<SYS>(lambda)`, que obtiene la caché del sistema.
- El `SlotMap` pasa de `std::array` a `std::vector` y crece al llenarse (`resize()` duplica la capacidad y enlaza los slots nuevos a la freelist). Al revisarlo he corregido varios bugs:
  - `operator[]` devolvía `data_[key.id]` en lugar de `data_[indices_[key.id].id]`, así que tras un `erase` devolvía el elemento de otra key.
  - `resize()` enlazaba mal la freelist.
  - `clear()` no reiniciaba `size_` ni invalidaba las keys antiguas. Ahora cada slot recibe una generación nueva al limpiar.
  - La generación empieza en 1, para que una key por defecto `{0,0}` nunca sea válida.
- Sacada la key del slotmap a un tipo común `Key`, igual para todos los slotmaps, para que un componente pueda guardar la key de otra entidad sin depender del tipo del manager.
- `Entity` guarda las keys de sus componentes en un `std::array<Key, N>` indexado por `cmpinfo::id<CMP>()` en lugar de en una tupla.
- Añadido `getEntityById`, que devuelve `Entity*` (o `nullptr` si no existe).

### Decisiones de diseño
- Las cachés se indexan por firma, no por sistema: dos sistemas con la misma firma comparten caché.
- Los sistemas no deberían tener estado. De momento el índice de la caché se guarda en un `static` del sistema; hay que sustituirlo por una forma en la que sea el manager quien sepa qué caché corresponde a cada sistema.
- `addComponent` sigue devolviendo una referencia (válida hasta el siguiente cambio estructural en ese storage), no la key. La key no le sirve al usuario: para acceder al componente le basta con la entidad y `getComponent`.
- Para que un componente apunte a otro (por ejemplo, un `CAttach` al `CTransform` de su padre) se guardará la key de la entidad, no una referencia ni un puntero: es estable aunque el slotmap crezca o mueva datos, y la generación detecta si el padre ya no existe.

### Problemas / bloqueos
- Con las cachés guardando `id`s y las entidades en un `std::vector`, resolver cada `id` en el `foreach` requiere una búsqueda lineal y el recorrido acaba siendo O(n²). Hay que pasar las entidades a un slotmap y guardar keys en las cachés.

### Aprendizajes
- `"texto" + entero` no concatena: es aritmética de punteros sobre el `const char*`. Hay que usar `std::to_string` o `std::format` (con `{:#b}` para ver una máscara en binario).
- En la definición fuera de clase de un método, el tipo de retorno se lee antes de entrar en el ámbito de la clase: hay que calificarlo con `typename EntityManager<...>::Entity` o usar un tipo de retorno al final (`auto ... -> T`).
- `std::reference_wrapper` no puede estar vacío; para expresar "no encontrado" sirve un puntero o un `std::optional<std::reference_wrapper<T>>`.
- Las lambdas se reciben como parámetro de plantilla (`auto&&` o `F&&`) para que el compilador pueda hacerlas inline; `std::function` añade una llamada indirecta.
- `std::get<Tipo>` sobre una tupla falla si el tipo aparece más de una vez; con todas las keys del mismo tipo, un `std::array` indexado es lo natural.
- Al pasar un contenedor de `std::array` a `std::vector`, `reserve` no crea elementos (hace falta `resize`), y las referencias a los elementos se invalidan cada vez que el vector crece.

### Próximos pasos
- Actualizar el almacenamiento de entidades de `std::vector` a `SlotMap`.
- Terminar el `foreach`.
- Actualizar las cachés en todos los casos que les afectan: `addComponent`/`addTag`, `removeComponent`/`removeTag` y `removeEntity`.

---

## 2026-10-17

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 2h

### Qué he hecho hoy
- Creado un sistema de log (`log.hpp`/`log.cpp`): `Log::printTrace` imprime el mensaje junto al fichero, la línea, la columna y la función de la llamada con `std::source_location`, y `Log::printNoTrace` imprime solo el mensaje.
- `Log::initialize()` redirige `std::clog` a un fichero por ejecución, `log/execution/<dd-mm-aaaa-HHh-MMm-SSs>_log.txt`. Crea la carpeta si no existe y, si no puede abrir el fichero, avisa por `std::cerr` y deja `clog` en la terminal. Al cerrar el programa, el destructor restaura el buffer original de `clog`.
- `runTimeAssertion`/`debugAssertion` aceptan un mensaje opcional y muestran la expresión que ha fallado. `warning()` también acepta un mensaje opcional. Ambos usan `std::source_location` como parámetro por defecto, así que la traza apunta a la línea donde se llama a la macro y no a `warning.cpp`.
- Corregidas llamadas a los asserts: faltaba una coma en `slotmap.hpp` y un paréntesis mal cerrado en `ecs.hpp` hacía que el mensaje se descartara sin error.
- Añadido `#include <cstdint>` en `slotmap.hpp`: usaba `std::uint32_t` y solo compilaba porque le llegaba indirectamente desde otra cabecera.
- `log/execution` añadido al `.gitignore`.

### Decisiones de diseño
- El log escribe en `std::clog` y no en `std::cout`, para no mezclarse con la salida normal del programa y poder redirigirlo de forma independiente.
- `Log` es un singleton creado con un `static` local dentro de `initialize()`. Como se construye después que el `ofstream` global, se destruye antes que él y restaura `clog` antes de que se cierre el fichero.
- Un fichero de log por ejecución, con fecha y hora en el nombre, en lugar de sobrescribir siempre el mismo.

### Problemas / bloqueos
- Ninguno

### Aprendizajes
- Diferencia entre `std::cout`, `std::clog` y `std::cerr`: `cout` va a stdout y `clog`/`cerr` a stderr. `cout` y `clog` tienen buffer y `cerr` no. Dos streams con buffers distintos pueden mostrar sus mensajes desordenados.
- Se puede redirigir un stream cambiando su buffer con `rdbuf()`. El `ofstream` tiene que vivir más que la redirección, o el stream acaba usando un buffer destruido.
- Un argumento por defecto `std::source_location::current()` se evalúa en el sitio de la llamada, y eso es lo que permite que la traza apunte a la línea que llama al assert.
- Una variable `static` definida en una cabecera crea una copia en cada `.cpp` que la incluye. Los detalles internos van en un namespace anónimo dentro del `.cpp`.
- Una función definida en una cabecera tiene que ser `inline` para no violar la ODR (One Definition Rule).
- `ofstream` no crea directorios: si la carpeta no existe, `open` falla sin avisar.

### Próximos pasos
- Realizar el precacheado para los sistemas: que el `foreach` recorra un array precacheado con las entidades que cumplen la firma del sistema.

---

## 2026-10-16

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 2h

### Qué he hecho hoy
- Añadidos `addTag`/`removeTag` y `addTags`/`removeTags` en `EntityManager`, con su versión privada en `Entity` que actualiza la máscara de tags (`|=` para añadir, `&= ~mask` para quitar).
- `Entity` declara `friend class EntityManager` y deja privados sus `addComponent`/`removeComponent`/`addTag`/`removeTag`: solo el manager puede modificar las máscaras, así no se desincronizan del slotmap.
- Añadido `getComponent<CMP>(e)`, que devuelve una referencia al componente de la entidad, con una sobrecarga `const` del manager que devuelve `const CMP&`. Ambas reciben `const Entity&`.
- Añadidas las sobrecargas `const` de `ComponentStorage::getStorage<CMP>()` y `SlotMap::operator[]` para soportar el `getComponent` const.
- Corregido `addTags`/`removeTags`: el fold `(debugAssertion(...), ...)` no compilaba en release, porque la macro expandía a `static_cast<void>(0)` sin el pack. Ahora delegan en `addTag<TAGs>(e)`/`removeTag<TAGs>(e)`, que ya llevan el assert.

### Decisiones de diseño
- `getComponent` recibe la entidad como `const` en ambas sobrecargas: la entidad solo guarda la máscara y la clave, y el componente vive en el storage del manager. La constness del componente devuelto la decide el manager, no la entidad.
- Las versiones con pack (`addTags`/`removeTags`) delegan en las individuales para no duplicar el assert.

### Problemas / bloqueos
- Ninguno

### Aprendizajes
- No meter un parameter pack dentro de una macro que puede desaparecer en release: si la expansión deja de mencionar el pack, la fold expression no compila.
- Para devolver `const T&` desde un método `const`, toda la cadena de acceso (storage, slotmap) necesita su sobrecarga `const`.

### Próximos pasos
- Realizar el precacheado para los sistemas: que el `foreach` recorra un array precacheado con las entidades que cumplen la firma del sistema.

---

## 2026-10-14

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 4h

### Qué he hecho hoy
- Arreglada la definición fuera de clase de `addComponent` (faltaba separar la cabecera `template<>` de la clase y la del método, por eso la declaración era incompatible).
- Añadidos `addComponents` y `deleteComponent`/`deleteComponents`, que expanden un parameter pack de tipos con fold expressions (`(f<CMPs>(...), ...)`) y usan `std::forward` para preservar la categoría de valor de cada componente.
- Implementado `type_at<i, Ts...>` (y `type_at_t`) en `typetraits.hpp`, con `static_assert` de rango, y expuesto como `Typelist::at<i>` y `type_traits::at<i>`.
- `removeEntity` ahora elimina todos los componentes de la entidad: `removeEntityimp` recorre `std::make_index_sequence<cmpinfo::list_size>` y, para cada índice, llama a `removeIfPresent<cmpinfo::at<I>>(e)` mediante una fold expression (sin lambda).
- Corregido `removeComponent`: la clave se leía sin inicializar; ahora se obtiene con `getComponentKey<CMP>()`. `Entity::removeComponent` limpia el bit con `&= ~mask` en lugar de invertirlo con `^=`.
- `addComponent` pasa a usar forwarding reference con `std::remove_cvref_t<CMP>` para el tipo real del componente, de modo que acepta lvalues y rvalues (copia o mueve en el `push_back` del slotmap). `addComponents` deduce los tipos sin `<CMPs>` explícito.
- Documentado `typetraits.hpp` con comentarios estilo Doxygen.

### Decisiones de diseño
- Definir los métodos plantilla de `EntityManager` fuera de la clase con doble cabecera `template<typename CMPS, typename TAGS, std::size_t Capacity> template<typename CMP>`, manteniendo el patrón ya usado para `createEntity`/`addEntity`.
- Sustituir la lambda de `removeEntityimp` por un método plantilla `removeIfPresent<CMP>`, que deja el fold en una sola expresión y es más legible.
- `addComponent` no se llama con el tipo explícito (`addComponent<T>(e, x)`): con el tipo fijado a mano, `CMP&&` deja de ser forwarding reference y no admite lvalues.

### Problemas / bloqueos
- Ninguno

### Aprendizajes
- Diferencia entre `std::move` y `std::forward` en forwarding references: `forward` reenvía la categoría de valor original (lvalue/rvalue) según el tipo deducido del pack, `move` la fuerza siempre a rvalue.
- Un índice runtime no puede usarse para indexar un parameter pack de tipos: el tipo con el que se instancia una plantilla debe resolverse en compile-time.
- Con `std::index_sequence<I...>` no hay un array de índices en runtime, sino un pack de constantes compile-time; cada `I` sirve para instanciar `at<I>`.
- `T&&` solo es forwarding reference cuando `T` se deduce; con el tipo explícito es una referencia a rvalue y no enlaza con lvalues.

### Próximos pasos
- Crear `addTag`/`removeTag`.
- Crear `getComponent<CMP>(e)` que devuelve una referencia al componente de dicha entidad.

---

## 2026-09-23

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 6h

### Qué he hecho hoy
- ComponentStorage realizado, el tipo del almacenamiento se deduce en tiempo de compilacion, metafunciones para devolver el storage de cada componente
- En el entitymanager he diseñado el addComponent, que añade un componente nuevo al slotmap asi como en la entidad (Actualizando la mascara y añadiendo la Key)

### Decisiones de diseño
- Añadir un componente ya existente salta un warning y devuelve el componente actual de la entidad.

### Problemas / bloqueos
- Ninguno

### Aprendizajes
- Flujo de manejo de errores 

### Próximos pasos
- Borrar componente, asi como borrar entidad(importante borrar todos los cmps de la entidad).
- El forech() precacheado por la firma de los sistemas.

---

## 2026-09-18

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 6h

### Qué he hecho hoy
- Metaprogramar para deducir directamente el tipo del Storage: std::tuple<Slotmap<Cmps, Capacity> ....>,
- Type_traits para componentes y tags, que permiten asignar una id a cada una.

### Decisiones de diseño
- Tupla como almacenador de Slotmaps.

### Problemas / bloqueos
- Ninguno

### Aprendizajes
- Repaso sobre templates y metaprogramacion

### Próximos pasos
- Crear el componentStorage y permiter asociar los identificadors de cada cmps, tag a las entidades del manager.

---

## 2026-09-14

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 3h

### Qué he hecho hoy
- Implementar el Slotmap para el dummy ECS

### Decisiones de diseño
- ECS con array(tamaño estatico) se podria haber usado vector, pero como lo que se quiere es hacerlo lo mas simple posible.

### Problemas / bloqueos
- Ninguno

### Aprendizajes
- Repaso sobre que era un Slotmap y su funcionamiento interno

### Próximos pasos
- Metaprogramacion para tener los cmps traits y poder actualizar las mascaras de nuestras entidades.

---

## 2026-09-09

**Fase:** Fase1 - ECS basico

**Tiempo invertido:** 1h

### Qué he hecho hoy
- Creada la rama sw/ecs/basic/1.0 Donde se desarrolla un ECS basico con slotmaps.
- Diseñar la carcasa de EntityManager.

### Decisiones de diseño
_(elecciones tomadas y por qué, alternativas descartadas)_
- Entity sera un struct dentro de EntityManager. Las entidades viven para el EntityManager
- Usar template para pasar la lista de componentes existentes a la clase.

### Problemas / bloqueos
- Necesito desarrollar el almacenamiento para los componentes (Slotmaps).
- Necesito metaprogramacion para las CMPs traits que mas adelante usaremos para gestionar nuestras entidades.

### Aprendizajes
_(algo nuevo entendido sobre DOD, arquetipos, concurrencia, etc.)_
- NA (Ya hice esto en la carrera) 

### Próximos pasos
- Ver los videos de Francisco Gallego Duran (Profesor Retroman), sobre [Slotmaps](https://www.youtube.com/watch?v=ZfWF9tqQOh0) y [metaprogramcion](https://www.youtube.com/watch?v=4NKbmCfZ9OI).
- Una vez vistos los videos, proceder a implementarlo en este ECS basico.

---
## 2026-09-07

**Fase:** Fase0 - Puesta en marcha

**Tiempo invertido:** 1h
### Qué he hecho hoy
- He visualizado la conferencia de  Vittorio Romeo en la CppCon 2025 sobre [Practical Data-Oriented Design in C++](https://www.youtube.com/watch?v=SzjJfKHygaQ)

### Decisiones de diseño
- NA

### Problemas / bloqueos
- NA

### Aprendizajes
- He aprendido sobre el diseño orientado a datos:
    - Los datos estan siempre expuestos.
    - Se centra en la actualizacion del estado de los mismos datos.
    - El bucle es el que decide como se actualizan dichos datos.
    - Estudiar el hardware donde piensas trabajar para poder exprimirlo al maximo.
    - Suprimir herencia/single heap allocations.

### Próximos pasos
- Implementar un ECS sencillo empleando el DOD (Data oriented desing).

## 2026-09-05

**Fase:** Fase0 - Puesta en marcha

**Tiempo invertido:** 2h

### Qué he hecho hoy
- Instalado herramientas necesarias (Msys, Camke, mingw... )
- Configurado CMajeLists.txt para compilar de manera sencilla el proyecto
- Creado ecs.hpp/ecs.cpp y main.cpp como prueba
- Todo compila y se ejecuta sin problema.

### Decisiones de diseño
- Usar Cmake debido a su facilidad y ser ampliamente usado para librerias
- Estructura del proyecto carpeta code/source + code/include
- Se usara la version de C++ 23

### Problemas / bloqueos
- Instalar todo lo necesario para poder compilar C++ y usar cmake.
- Decidir la estructura de la libreria

### Aprendizajes
- Como funciona Cmake, añadir librerias, archivos de cabecera, flags...

### Próximos pasos
- Investigar sobre el DOD (data oriented desing)
- Primer prototipo de ECS sencillo

---
## 2026-09-05

**Fase:** Fase 0 — Puesta en marcha.

**Tiempo invertido:** 3h

### Qué he hecho hoy
- Creada la organización `heatstroke-engine` en GitHub.
- Repositorio principal `HeatStroke` creado.
- Definida la licencia (MIT + cláusula de atribución).
- Repo del ECS creado como `heatstroke-ecs`.

### Decisiones de diseño
- Cada subsistema del motor tendrá su propio repo bajo la organización, prefijado con `heatstroke-` para mantener consistencia.

### Problemas / bloqueos
- Ninguno relevante hoy, trabajo organizativo más que técnico.

### Aprendizajes
- Diferencia entre licencias OSI puras (MIT/Apache) y licencias con cláusula de atribución tipo BSD-4-Clause.

### Próximos pasos
- Preparar el entorno para poder compilar C++ empleando CMAKE.

---
