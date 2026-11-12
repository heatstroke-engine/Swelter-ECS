# Decisiones de diseño — HeatStroke ECS

Registro de las decisiones de diseño importantes del ECS: qué se decidió, por qué, qué alternativas se descartaron y qué consecuencias tiene. El diario (`log/diary/log.md`) cuenta el día a día; este documento recoge solo las decisiones que condicionan la arquitectura, para poder justificarlas en la memoria.

El ECS de arquetipos (fase 2 en adelante) se diseña desde cero: **las decisiones del ECS dummy (fase 1) no se heredan**. Se conservan como histórico y como punto de partida de la comparación.

**Criterio general:** al ser un proyecto académico, en algunos casos se elige la opción más sencilla, siempre que no comprometa la fase 3 (multihilo). Las alternativas más elaboradas quedan anotadas como mejoras futuras.

**Estados:** `Abierta` (pendiente de decidir) · `Aceptada` · `Sustituida por D-XX` (se mantiene para ver la evolución) · `Histórico` (ECS dummy).

Formato de cada decisión:

```
### D-XX — Título
- **Estado / Fase / Fecha:**
- **Contexto:** el problema y las restricciones.
- **Opciones:** alternativas consideradas, con pros y contras.
- **Decisión:** lo elegido y por qué.
- **Consecuencias:** lo que implica, incluido lo malo. Datos de benchmarks si los hay.
```

## Índice

| ID | Decisión | Fase | Estado |
|---|---|---|---|
| H-01 | Herramientas: C++23 y CMake | 0 | Histórico |
| H-02 | Componentes y tags en una lista cerrada en compilación (`Typelist`) | 1 | Histórico |
| H-03 | Almacenamiento: un slotmap por componente | 1 | Histórico |
| H-04 | La API pública trabaja con keys | 1 | Histórico |
| H-05 | Cachés de entidades por firma de sistema | 1 | Histórico |
| D-01 | Metodología de benchmarks y comparación con EnTT | 1–3 | Aceptada |
| D-02 | Modelo de almacenamiento: arquetipos | 2 | Aceptada |
| D-03 | Registro de componentes: tipos de C++ abiertos con ID en ejecución | 2 | Aceptada |
| D-04 | Identificación de arquetipos: lista ordenada de IDs + bitset | 2 | Aceptada |
| D-05 | Almacenamiento de un arquetipo: chunks de tamaño fijo con layout SoA | 2 | Aceptada |
| D-06 | Tags: forman parte del arquetipo | 2 | Aceptada |
| D-07 | Transiciones entre arquetipos: grafo con aristas cacheadas | 2 | Aceptada |
| D-08 | Queries: caché de arquetipos con actualización incremental | 2 | Aceptada |
| D-09 | Cambios estructurales diferidos: command buffers explícitos | 2 | Aceptada |
| D-10 | Paralelismo: job system y planificación de sistemas | 3 | Abierta |
| D-11 | IDs de entidad y tabla de ubicaciones paginada | 2 | Aceptada |
| D-12 | Errores: sin excepciones, aserciones en debug y `noexcept` solo donde aporta | 2 | Aceptada |

---

## Histórico — ECS dummy (fases 0 y 1)

Resumen de lo que se hizo en el ECS dummy. No condiciona el ECS de arquetipos; sirve para explicar de dónde se parte y qué problemas motivan el nuevo diseño.

### H-01 — Herramientas: C++23 y CMake
C++23 sin extensiones y CMake con presets (`debug` / `release`). Dependencias externas (Google Benchmark, EnTT) con `FetchContent` y versión fijada.

### H-02 — Componentes y tags en una lista cerrada en compilación (`Typelist`)
`EntityManager<CMPS, TAGS, Capacity>` recibe las listas de tipos; cada tipo tiene un índice y un bit de máscara en compilación. Sin coste en ejecución para identificar tipos, pero todos los componentes deben listarse en el tipo del mundo.

### H-03 — Almacenamiento: un slotmap por componente
Una tupla de slotmaps, uno por componente; la entidad guarda su máscara y la key de cada componente. Iterar varios componentes salta entre slotmaps (entidad → key → slotmap → dato): `ITERATETWOCMPS` es ~×8 más lento que EnTT, y `ITERATETWOCMPSHALF` tarda casi lo mismo que `TWOCMPS` recorriendo la mitad de entidades.

### H-04 — La API pública trabaja con keys
La API recibe keys con generación en lugar de referencias, que se invalidan si el storage crece o mueve datos. La key detecta además entidades destruidas.

### H-05 — Cachés de entidades por firma de sistema
Cada firma registrada guarda las entidades que la cumplen y se actualiza tras cada cambio estructural. Sacar una entidad de una caché es un `std::erase` lineal, así que los cambios estructurales son O(n²): con 100K entidades, `ADDREMOVECMPCACHED` es ×474 y `DESTROYENTITIESCACHED` ×365 más lento que EnTT.

---

## Transversal

### D-01 — Metodología de benchmarks y comparación con EnTT
- **Estado / Fase / Fecha:** Aceptada · Fases 1–3 · 2026-10
- **Contexto:** las decisiones deben poder justificarse con datos comparables entre versiones y frente a una referencia.
- **Decisión:**
  - Google Benchmark, siempre en release, 10 repeticiones, de 1K a 1M entidades. Los resultados se guardan en JSON con los datos de la máquina.
  - Si el test cambia la estructura del mundo, se reconstruye en pausa en cada iteración; si no, se construye una vez.
  - EnTT v4.0.0 como referencia, con los mismos tests y componentes. Un sistema con caché equivale a un *owning group*, y además se mide con *views*.
  - Comparar arquetipos con 1 hilo frente a EnTT (aporte del diseño) y arquetipos con N hilos frente a 1 hilo (aporte del paralelismo).
- **Consecuencias:** cada fase nueva ejecuta la misma batería para ver su evolución. Informes con `benchmark_to_latex.ps1` y `compare_benchmarks_to_latex.ps1`.

---

## Fase 2 — ECS de arquetipos (single thread)

### D-02 — Modelo de almacenamiento: arquetipos
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** objetivo del TFG y resultados del dummy (H-03, H-05). Caso de uso: un RTS 2D, con muchas entidades en pocas combinaciones de componentes que se recorren cada frame.
- **Opciones:** sparse sets (EnTT) frente a arquetipos (Unity DOTS, flecs, Bevy).
- **Decisión:** arquetipos. Las entidades con la misma combinación de componentes se guardan juntas y contiguas.
- **Consecuencias:** la iteración es contigua y se paraleliza bien, porque los bloques se reparten entre hilos. Añadir o quitar componentes es más caro, porque mueve la entidad de arquetipo; lo mitigan D-07 y D-09.

### D-03 — Registro de componentes: tipos de C++ abiertos con ID en ejecución
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** hay que decidir qué puede ser un componente y cómo se identifica.
- **Opciones:**
  - **A. Lista cerrada en compilación** (como el dummy): máxima información en compilación, pero el mundo debe conocer todos los componentes, los módulos no pueden añadir los suyos y añadir uno recompila todo.
  - **B. Tipos de C++ abiertos con ID asignado en ejecución** (EnTT, Bevy, Unity DOTS): cualquier `struct` es un componente sin listarlo antes; la primera vez que se usa recibe un ID de un contador.
  - **C. Componentes totalmente dinámicos** (flecs, Bevy `ComponentDescriptor`): tipos sin equivalente en C++, descritos por tamaño, alineación y funciones. Necesarios para scripting, editor, mods, relaciones al estilo flecs o tags creados en ejecución.
- **Decisión:** **B**. Cada tipo se describe internamente con un `ComponentInfo` (tamaño, alineación, funciones de mover y destruir) que C++ rellena automáticamente. C queda fuera del TFG; el `ComponentInfo` es la única puerta que se deja entreabierta.
- **Consecuencias:**
  - En un ECS de arquetipos el almacenamiento es type-erased con cualquier opción (las combinaciones de componentes se crean en ejecución), así que B no cuesta nada en iteración: la query resuelve los punteros a columna una vez por bloque y el bucle es tipado.
  - Los IDs de componente no son `constexpr` y no tienen un límite fijo, lo que descarta las máscaras de ancho fijo como identificador de arquetipo (D-04).
  - El registro de tipos tiene que ser seguro con varios hilos, o hacerse antes de que empiece el trabajo en paralelo (fase 3).

### D-04 — Identificación de arquetipos: lista ordenada de IDs + bitset
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** al crear una entidad o cambiar sus componentes hay que encontrar su arquetipo, y las queries tienen que saber qué arquetipos encajan con su firma. Con D-03 el número de tipos no está acotado.
- **Opciones:**
  - **Máscara de ancho fijo** (`uint64_t`): comparar, hacer hash y encajar queries es trivial, pero limita el número de componentes. Descartada por D-03.
  - **Lista ordenada de IDs** (flecs, Bevy, Unity): forma canónica (el mismo conjunto produce la misma lista sin importar el orden en que se añadieron los componentes), ocupa según los componentes del arquetipo y se puede hacer hash.
  - **Bitset dinámico**: encajar una query sigue siendo un AND, pero su tamaño depende del total de tipos registrados.
- **Decisión:** la **identidad** del arquetipo es la lista ordenada de IDs de componente. Cada arquetipo guarda además un **bitset auxiliar** con los mismos componentes, solo para encajar queries rápido (`(arch & query) == query`).
- **Consecuencias:**
  - La búsqueda de lista a arquetipo solo ocurre en el camino frío (combinaciones nuevas); iterar y acceder a componentes no la usa. La estructura de búsqueda se elegirá midiendo.
  - Como los IDs son un contador (pequeños y densos), el bitset es compacto.
  - Los arquetipos se guardan en un vector y se referencian por índice (`ArchetypeId`).
- **Implementación (2026-11-13, #16):**
  - **Camino frío: `std::unordered_map` con el bitset como clave.** El registro limita los tipos a `MAX_COMPONENTS` (256), así que el bitset tiene ancho fijo, es único para cada conjunto de IDs y la librería estándar ya sabe hacerle hash. Evita hacer hash de la lista y compararla. Solo se usa cuando no hay arista (D-07), así que no se ha medido; si un benchmark lo pidiera, se puede cambiar sin tocar el resto.
  - **El vector guarda `std::unique_ptr<Archetype>`**: un arquetipo no se puede copiar ni mover, y así su dirección no cambia cuando el vector crece mientras se crean arquetipos nuevos.
  - **El arquetipo raíz** (sin componentes) se crea con el mundo y siempre tiene el ID 0. Toda entidad sale de él.

### D-05 — Almacenamiento de un arquetipo: chunks de tamaño fijo con layout SoA
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** cómo se guardan las filas de un arquetipo. El diseño tiene que encajar con el job system de la fase 3.
- **Opciones:**
  - **Una columna creciente por componente** (flecs, Bevy): totalmente contiguo, sencillo y sin desperdicio en arquetipos pequeños; pero al crecer realoja y copia la columna entera (tirones de frame) y los punteros no son estables.
  - **Chunks de tamaño fijo** (Unity DOTS): bloques de memoria iguales con las columnas de un número fijo de filas.
- **Decisión:** **chunks de tamaño fijo**, 16 KB por defecto, por su encaje con el multihilo:
  - el chunk es la unidad natural de trabajo de un job;
  - crecer es añadir un chunk, sin realojar ni copiar (sin tirones);
  - la memoria no se mueve y el allocator es un pool de bloques iguales;
  - permite metadatos por chunk (por ejemplo, versiones para *change detection*).
- **Mitigación de riesgos (fragmentación y desperdicio):**
  - **Política de borrado b:** el hueco de una fila borrada lo tapa la **última fila del último chunk** del arquetipo. Todos los chunks están llenos menos el último: sin fragmentación interna y con un desperdicio máximo de un chunk por arquetipo.
  - **Pool global de chunks libres:** un chunk vacío vuelve al pool y lo reutiliza cualquier arquetipo. Al tener todos el mismo tamaño, no hay fragmentación externa.
  - **Peor caso acotado:** desperdicio ≤ número de arquetipos × tamaño de chunk (200 arquetipos × 16 KB ≈ 3,2 MB).
  - **Componentes muy grandes:** capacidad mínima de filas por chunk; si no se alcanza, ese arquetipo usa chunks mayores.
  - **Tamaño de chunk como parámetro** (`ChunkSize`) para comparar 16, 32 y 64 KB en los benchmarks.
- **Consecuencias:**
  - La iteración es un bucle doble (chunks y filas). La query entrega **bloques** (punteros a columnas + número de filas), que es también lo que verá el job system.
  - Borrar una fila mueve la última fila del arquetipo, que puede estar en otro chunk, y obliga a actualizar la ubicación de la entidad movida.
  - Los arquetipos con muy pocas entidades desperdician casi un chunk entero; los multiplican sobre todo los tags que cambian a menudo (D-06).
- **Layout dentro del chunk:**
  - **Opciones:**
    - **AoS** (filas contiguas): acceder a una entidad toca una línea y mover una fila es copiar un bloque, pero al iterar se carga todo aunque el sistema use pocos componentes (`RenderSystem` aprovecharía 1 de cada 28 bytes en el ejemplo `Entity + CTransform + CPhysics + CRender`).
    - **SoA** (una columna por componente): cada sistema lee solo sus columnas, sin desperdiciar bytes de las líneas de caché, con flujos secuenciales para el prefetcher y bucles autovectorizables. A cambio, acceder a varios componentes de una entidad toca varias líneas y mover una fila es una copia por columna.
    - **AoSoA** (bloques de K filas con SoA dentro): solo aporta con vectorización manual por campo. Un chunk SoA ya es un AoSoA con K = la capacidad del chunk.
  - **Decisión: SoA por componente** (no por campo: los componentes son type-erased y el ECS no conoce sus campos). Es el layout de Unity DOTS, flecs y Bevy, y separar componentes calientes y fríos sale gratis.
    - **Orden de columnas:** columna de entidades primero; después, los componentes en orden de ID, así que la posición en la lista ordenada (D-04) es el índice de columna y no hace falta ningún mapa.
    - **Alineación:** cada columna empieza alineada a 64 B (línea de caché): sin líneas compartidas entre columnas (evita *false sharing* en la fase 3) y accesos alineados para SIMD. Coste: < 64 B por columna.
    - **Capacidad y desplazamientos** calculados una vez por arquetipo: `N = ChunkSize / suma de tamaños`, reduciendo N si no cabe con el relleno; `off[i] = align_up(off[i-1] + N * size[i-1], 64)`.
    - **Cabecera fuera del chunk:** número de filas, versiones, etc. en un array de descriptores del arquetipo. Los 16 KB son solo datos, y los descriptores juntos permiten saltarse chunks (*change detection*) sin cargar su memoria.
  - **Riesgo conocido:** *4K aliasing*. Si una columna que se lee y otra que se escribe están separadas por un múltiplo de 4096 B, la CPU frena las lecturas. Medirlo con un benchmark y, si aparece, desplazar las columnas.

### D-06 — Tags: forman parte del arquetipo
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** un tag no tiene datos; hay que decidir si "la entidad lo tiene" forma parte de la identidad del arquetipo. En un RTS hay tags que casi nunca cambian (`TPlayer`, `TEnemy`, `TBullet`) y otros que cambian a menudo (`TSelected`, `TStunned`).
- **Opciones:**
  - **En el arquetipo**, como componente de tamaño 0 sin columna (flecs, Bevy, Unity DOTS): filtrar sale gratis (se decide por arquetipo, sin `if` por entidad) y no ocupa memoria; pero poner o quitar un tag es un cambio estructural (mover la fila de arquetipo) y los tags multiplican los arquetipos (hasta ×2^k con k tags).
  - **Fuera del arquetipo**, como bit por fila en el chunk (el dummy): cambiar un tag es escribir un bit y se puede hacer dentro de un job; pero filtrar recorre todas las filas con un `if` por entidad (el caso `ITERATETWOCMPSHALF`) y no permite saltarse chunks.
  - **Híbridos:**
    - *enableable components* de Unity: el tipo está en el arquetipo y el chunk lleva una columna de bits de activado más un contador por chunk;
    - *toggle* y componentes que no fragmentan en flecs;
    - componentes en sparse set en Bevy, que cambian el arquetipo pero no la tabla;
    - *shared components* de Unity: un valor por chunk, por ejemplo la facción.
- **Decisión:** **el tag forma parte del arquetipo.** Un tipo vacío (`std::is_empty_v<T>`) es un componente sin columna: está en la lista ordenada y en el bitset (D-04), pero no ocupa espacio en el chunk.
- **Consecuencias:**
  - Los tags que cambian a menudo son caros (cambio estructural) y fragmentan en arquetipos. Recomendación de uso: tags para propiedades que se fijan al crear la entidad.
  - En la fase 3, poner o quitar un tag desde un job tendrá que diferirse con un command buffer (D-09).
- **Mejora futura (al final de la fase 2 o en la fase 3, si da tiempo):** componentes y tags **activables** al estilo de Unity: un rasgo por tipo, una columna de bits de activado en el chunk y un contador de activados en el descriptor del chunk. Activar o desactivar no sería un cambio estructural. Benchmark previsto: alternar `TSelected` en 10K entidades como cambio estructural frente a hacerlo con un bit.
- **Trabajo futuro (fuera del TFG):** *shared components* (por ejemplo, la facción como valor por chunk).

### D-07 — Transiciones entre arquetipos: grafo con aristas cacheadas
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** añadir o quitar componentes o tags, o crear una entidad con varios, cambia su combinación y por tanto su arquetipo. Una transición consiste en:
  1. encontrar (o crear) el arquetipo destino;
  2. reservar una fila en él;
  3. mover las columnas comunes;
  4. construir los componentes nuevos o destruir los quitados;
  5. tapar el hueco en el origen con la política b (D-05);
  6. actualizar la ubicación de la entidad y la de la que tapa el hueco (D-11).
- **Opciones:**
  - **Calcular y buscar cada vez:** construir la lista nueva, hacer hash y buscar en el mapa. Correcto, pero se paga en cada cambio estructural.
  - **Grafo de arquetipos** (flecs, Bevy): cada arquetipo cachea las aristas "añadir C → X" y "quitar C → Y". Tras la primera vez, la transición es O(1).
  - **Dónde guardar las aristas:**
    - array indexado por ID: O(1), pero casi vacío y del tamaño del total de componentes;
    - vector pequeño `(id, destino)` con búsqueda lineal;
    - mapa hash;
    - híbrido de flecs: array para IDs bajos y mapa para altos.
- **Decisión:**
  - **Grafo con aristas cacheadas en los dos sentidos** (`A.add[C] = B` y `B.remove[C] = A`). Si la arista no existe, se construye la lista y se busca en el mapa, solo la primera vez.
  - **Aristas guardadas en un vector pequeño `(id, destino)`** por arquetipo. Medirlo y, si hace falta, pasar al híbrido de flecs.
  - **Aristas por bundle** (Bevy) para cambios de varios componentes a la vez y para crear entidades directamente en su arquetipo final. Un paquete de tipos (`<CTransform, CPhysics, CRender>`) recibe un ID de bundle la primera vez que se usa, igual que los componentes (D-03). Una sola mudanza y sin arquetipos intermedios.
  - **Mudanza de datos:** camino rápido con `memcpy` para tipos trivialmente copiables (un flag en `ComponentInfo`); si no, constructor de movimiento en el destino y destructor en el origen.
  - **Chunks reservados solo al llegar la primera entidad:** los arquetipos vacíos apenas cuestan memoria.
- **Consecuencias:**
  - La API debe permitir crear y modificar entidades con varios componentes de golpe, y esa debe ser la forma recomendada. Añadirlos de uno en uno haría una mudanza por componente y crearía arquetipos intermedios.
  - Cada arquetipo nuevo hay que comprobarlo contra las queries existentes (D-08).
  - La correspondencia entre las columnas de origen y destino se calcula recorriendo las dos listas ordenadas a la vez. Se podría cachear en la arista si un benchmark lo pide.
- **Mejoras futuras:**
  - **Transiciones por lotes:** mover muchas entidades con la misma transición buscando el destino una vez y copiando rangos de filas.
  - **Cambio de chunk entero cuando solo cambian tags:** origen y destino tienen las mismas columnas de datos, así que un chunk completo puede pasar al otro arquetipo sin copiar datos (D-06).
- **Implementación (2026-11-13, #16):**
  - **Mudanza (`moveEntity`):**
    - Reserva primero la fila en el destino.
    - Recorre las dos listas ordenadas como en un merge: mueve los comunes, destruye los que solo están en el origen y deja sin construir los que solo están en el destino, que construye quien llama.
    - Cuando se acaba la lista del destino, su ID se sustituye por un centinela (`MAX_COMPONENTS`) mayor que cualquier ID real, así que los componentes que quedan en el origen caen en el caso "destruir" sin una rama aparte.
  - **`removeRow` dividido en dos:** `removeRow` destruye los componentes de la fila y llama a `fillHole`, que solo tapa el hueco con la última fila. La mudanza usa `fillHole` directamente, porque los componentes de la fila ya están movidos o destruidos y destruirlos otra vez sería una doble destrucción.
  - **Aristas de componente en los dos sentidos; aristas de bundle solo de ida.** La inversa de un bundle no es fiable: `{A} + {A,B} = {A,B}`, pero `{A,B} − {A,B} = {}`, no `{A}`. Las aristas de bundle van en listas separadas de las de componente, porque los IDs de componente y de bundle son espacios distintos con el mismo tipo base.
  - **Registro de bundles (`BundleRegistry`):**
    - Como el de componentes: un `static` local por combinación de tipos, que guarda los IDs ordenados la primera vez. Es el único `sort` que queda, y solo una vez por combinación.
    - Se quitan referencias y `const` antes de llegar a la función que guarda el `static`, para que `<const A&, B>` y `<A, B>` compartan bundle.
    - `<A,B>` y `<B,A>` sí son bundles distintos: llevan al mismo arquetipo y solo cuestan una arista más. Se prefirió a buscar la lista ordenada entre los bundles existentes.
  - **`create`, `add` y `remove` usan siempre bundles**, también con un solo tipo, así que todos los cambios de componentes son una sola mudanza.
  - **`add` de un componente que la entidad ya tiene:** se sobrescribe el valor sin mudanza, como el `insert` de Bevy, porque es la operación habitual de "poner este valor". Con un bundle que se solapa, los que ya tenía se asignan y los nuevos se construyen; se decide mirando el bitset del arquetipo de **origen** (el destino siempre los tiene todos). Añadir un tag que ya tiene no hace nada.
  - **`remove` de componentes que la entidad no tiene:** se ignoran. Si no tiene ninguno, warning en debug y no hay mudanza.
  - **Referencias invalidadas:** `add`, `remove` y `destroy` mueven filas, así que una referencia devuelta por `get` puede dejar de ser válida. Queda documentado en la API; D-09 evita el caso durante la iteración.

### D-08 — Queries: caché de arquetipos con actualización incremental
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** una query describe qué entidades quiere un sistema y cómo accede a sus datos. Debe encontrar los arquetipos que encajan, enterarse de los nuevos y entregar bloques (D-05).
- **Opciones:**
  - **Sin caché** (*views* de EnTT, queries no cacheadas de flecs): recorrer todos los arquetipos en cada ejecución. O(arquetipos) por sistema y frame.
  - **Con caché, push** (flecs): al crear un arquetipo, el mundo lo comprueba contra todas las queries registradas. Lista siempre al día, pero el mundo tiene que conocer todas las queries y crear un arquetipo cuesta O(queries).
  - **Con caché, pull incremental** (Bevy): como los arquetipos solo se añaden al vector, cada query recuerda hasta qué arquetipo ha mirado y, al ejecutarse, comprueba solo los nuevos. Si no hay nuevos, cuesta una comparación; sin registro ni avisos.
  - **Índice inverso componente → arquetipos** (flecs): acelera el primer encaje de una query nueva.
- **Decisión:**
  - **Términos:** requeridos, con modo **lectura o escritura** deducido de la `const`; con tag (`With<T>`); excluidos (`Without<T>`).
  - **Encaje por bitset** (D-04): `(bits & requeridos) == requeridos && (bits & excluidos) == 0`.
  - **Caché de arquetipos con actualización pull incremental.** Los arquetipos no se borran nunca (simplificación del TFG; un arquetipo vacío ocupa poco gracias a D-07).
  - **Por cada arquetipo que encaja se guardan las columnas resueltas** de cada término (su posición en la lista ordenada). Al iterar, el puntero base es `chunk + offset[columna]`, sin búsquedas.
  - **Iteración por chunk como base**: spans de cada término más las entidades; es la unidad de trabajo de la fase 3. `each` por entidad se construye encima. Se saltan arquetipos y chunks vacíos.
  - **Los modos de acceso se guardan en la query**, para la planificación de la fase 3 y una futura *change detection*.
- **Consecuencias:**
  - Las queries guardan arquetipos, no entidades: **los cambios estructurales nunca las tocan**. Crear una query cuesta O(arquetipos), no O(entidades). Debería eliminar el coste cuadrático de `ADDREMOVECMPCACHED` y `DESTROYENTITIESCACHED` del dummy (H-05) y abaratar `REGISTERSYSTEM`.
  - Si en el futuro se borran arquetipos, habrá que invalidar las listas de las queries.
  - En la fase 3, la actualización incremental no debe hacerse mientras otros hilos iteran la misma query: actualizar las queries antes de lanzar los jobs.
  - Dónde vive la query de cada sistema y cómo se declara se decide en la decisión de sistemas.
- **Mejoras futuras:** índice inverso componente → arquetipos, lista separada de arquetipos vacíos, términos opcionales (`T*`) y filtro `Changed<T>` con versiones por chunk.

### D-09 — Cambios estructurales diferidos: command buffers explícitos
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** un cambio estructural (crear, destruir, añadir o quitar componentes o tags) mientras se itera rompe la iteración:
  - el swap-and-pop (D-05) hace que se salten entidades o se visiten dos veces;
  - los spans pueden apuntar a chunks devueltos al pool;
  - los vectores de chunks o de arquetipos pueden realojarse.
  
  En la fase 3, además, sería una carrera de datos con otros hilos. Modificar **valores** de componentes no es un cambio estructural y se permite directamente.
- **Opciones:**
  - **Prohibirlo** con un assert durante la iteración: simple y seguro, pero cada sistema reinventa su propia cola.
  - **Permitirlo con reglas** (iterar hacia atrás, casos concretos como en EnTT): frágil e imposible de hacer seguro con hilos. Descartada.
  - **Diferirlo con un command buffer** y aplicarlo en un punto de sincronización: Unity (`EntityCommandBuffer`), Bevy (`Commands`), flecs (*deferred mode* automático).
  - **Buffer explícito** (Unity, Bevy) frente a **diferido automático** (flecs, misma API pero `has<T>` no ve el cambio hasta aplicarlo).
  - **Formato:** `std::vector<std::function>` (simple, pero reservas por comando y llamadas indirectas) frente a **buffer lineal de bytes** (Unity, flecs).
  - **Momento de aplicarlo:** al final de cada sistema, al final del frame, o en puntos de sincronización del planificador.
- **Decisión:**
  - **Command buffer explícito** (`cmd.create`, `cmd.destroy`, `cmd.add`, `cmd.remove`) **más un assert en debug** que prohíbe los cambios estructurales inmediatos mientras se itera (contador de iteración en el mundo). Fuera de las iteraciones, el mundo permite los cambios directos.
  - **Buffer lineal de bytes:** cada comando es una cabecera `{op, entidad, id de componente, tamaño}` seguida del valor. Los valores se mueven con el `ComponentInfo` (D-03). El buffer se vacía sin liberar memoria y se reutiliza entre frames.
  - **`cmd.create()` reserva el ID al momento** (D-11). Al aplicar, la entidad se materializa directamente en su arquetipo final, uniendo sus `add` en un solo bundle (D-07).
  - **Un buffer por sistema.** En la fase 2 se aplica al terminar el sistema. En la fase 3, en las barreras que decida el planificador, siempre en el orden de los sistemas. El sistema solo ve su `cmd`, así que cambiar el momento no afecta a los sistemas.
  - **Comandos sobre entidades muertas:** se comprueba la generación al aplicar; si la entidad ya no existe, el comando se ignora con un warning en debug.
- **Consecuencias:**
  - Los cambios hechos con `cmd` no son visibles hasta el punto de sincronización: dentro del mismo sistema, `has<T>(e)` sigue viendo el estado anterior.
  - **Requisito para la fase 3: resultado determinista.** Con buffers por hilo, el orden de aplicación no puede depender de la planificación: cada comando lleva una clave estable (por ejemplo, el índice del chunk, como la *sort key* de Unity) y se ordena antes de aplicar. Necesario para un RTS con lockstep determinista.
  - Cada punto de sincronización será una barrera en la fase 3: cuantos menos, más paralelismo.
- **Mejoras futuras:** agrupar comandos que hacen la misma transición (transiciones por lotes, D-07) y fusionar los comandos de una misma entidad en una sola mudanza.

### D-11 — IDs de entidad y tabla de ubicaciones
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-10-21
- **Contexto:** cada operación sobre una entidad (`get`, `add`, `remove`, `destroy`) y cada transición (D-07) necesitan saber dónde está: `entidad → {ArchetypeId, chunk, fila}`. Es la estructura más usada del ECS. Hay que decidir también cómo es el ID de una entidad y cómo se reutilizan los índices.
- **Opciones:**
  - **Formato del ID:** índice + generación. La generación detecta handles colgantes: un ID cuya generación no coincide con la de la tabla es de una entidad ya destruida.
    - **32 bits** (EnTT: 20 + 12): la mitad de memoria, pero 20 bits de índice son justo 1M entidades, y 12 bits de generación dan la vuelta tras 4096 reutilizaciones (ABA).
    - **64 bits** (Unity, Bevy: 32 + 32; flecs: 32 + 16 + flags): sin límites prácticos, a cambio de 8 B por entidad en la columna de entidades.
  - **Reutilización de índices:**
    - pila (LIFO): el registro reutilizado probablemente sigue en caché;
    - cola (FIFO): retrasa que la generación dé la vuelta; solo útil con generaciones pequeñas;
    - lista libre implícita (EnTT): la lista de libres vive dentro de la propia tabla, sin memoria extra.
  - **Ubicación guardada:**
    - **a)** `{ArchetypeId, chunk, fila en el chunk}`: acceso directo, unos 16 B por registro.
    - **b)** `{ArchetypeId, fila global}`, deduciendo el chunk con `fila / capacidad` gracias a la política b (D-05): 12 B, pero una división por acceso.
    - **Variante:** puntero al chunk (8 B, hay que actualizarlo si el chunk vuelve al pool).
  - **Almacenamiento de la tabla:**
    - vector plano (Bevy, Unity): simple, pero se realoja al crecer;
    - array paginado (flecs, EnTT): sin realojos grandes, y se puede reservar en paralelo sin que la tabla se mueva.
- **Decisión:**
  - **ID de 64 bits:** 32 de índice y 32 de generación. Un valor especial representa la entidad nula.
  - **Registro** `{generación, ArchetypeId, chunk, fila en el chunk}` (opción a, sin divisiones). Medir la opción b si la tabla resultara un cuello de botella.
  - **Tabla paginada desde el principio:** páginas de tamaño fijo reservadas según hacen falta.
  - **Lista libre implícita LIFO** dentro de la tabla. La generación sube al destruir, y un índice cuya generación llega al máximo se retira y no vuelve a usarse.
- **Consecuencias:**
  - Acceder a una entidad cuesta encontrar su página y su registro, comprobar la generación e ir a la fila del chunk. Sin hash ni divisiones (salvo la de la página, que es potencia de 2: un desplazamiento de bits).
  - El swap-and-pop de D-05 y las transiciones de D-07 actualizan dos registros: el de la entidad movida y el de la que tapa el hueco.
  - **Preparado para la fase 3:** reservar un ID y materializar la entidad deben poder separarse, para que un job reserve IDs (por ejemplo, con un contador atómico al estilo de Bevy) y la entidad se cree al aplicar el command buffer. Las páginas permiten reservar sin realojar la tabla.

### D-12 — Errores: sin excepciones, aserciones en debug y `noexcept` solo donde aporta
- **Estado / Fase / Fecha:** Aceptada · Fase 2 · 2026-11-13
- **Contexto:** al implementar el mundo (#16) aparecieron varios puntos donde algo puede fallar a mitad de una operación: superar `MAX_COMPONENTS`, agotar los índices de entidad, quedarse sin memoria al pedir un chunk o una página, o que la copia de un componente lance a mitad de `create` o `add`. Garantizar que el mundo queda en un estado coherente en todos esos casos complica el código (temporales, orden de las operaciones, deshacer a medias). Además hay que decidir qué funciones se marcan `noexcept`.
- **Opciones:**
  - **Excepciones con garantía fuerte:** si algo lanza, el mundo no cambia. Correcto, pero obliga a construir temporales antes de cada mudanza (un movimiento extra por componente) y a cuidar el orden de cada operación.
  - **Excepciones con garantía básica:** sin fugas, pero el mundo puede quedar a medias. Poco útil en un juego: no hay nada razonable que hacer después.
  - **Sin excepciones** (práctica habitual en motores de videojuegos): los errores de programación se comprueban con aserciones en debug, y los fallos irrecuperables, como quedarse sin memoria, terminan el programa.
- **Decisión:**
  - **Sin excepciones.** Ningún header del ECS lanza ni captura excepciones. Lo que antes era un `throw` (límite de `MAX_COMPONENTS` en el registro, índices de entidad agotados) es ahora un `debugAssertion` y una precondición documentada (`@pre ... checked only in debug`). Se puede compilar con `-fno-exceptions` (comprobado con el sandbox); activar el flag en CMake queda pendiente.
  - **En release no se comprueba nada:** si falla, el programa termina o el comportamiento es indefinido. Superar `MAX_COMPONENTS` termina en `std::bitset::set` (la librería estándar lanza `out_of_range` y, sin manejador, el programa termina). Agotar los 2^32 índices no se comprueba, pero no es un caso realista.
  - **`noexcept` solo donde tiene efecto o deja claro un contrato,** no en todas las funciones:
    - `-fno-exceptions` **no** hace que las funciones sean `noexcept`. Cambia la generación de código, pero `noexcept(f())`, `std::is_nothrow_move_constructible_v` y `std::move_if_noexcept` siguen dando lo mismo.
    - Importa en los constructores y asignaciones de movimiento, en `swap` y en los destructores: el concepto `Component` exige movimiento y destrucción `noexcept`, y `std::vector` copia en vez de mover al crecer si el movimiento no es `noexcept`.
    - Se pone también en funciones triviales que no reservan memoria (`getColumn`, `getEntities`, `valid`, `isAlive`...).
    - No se pone en las que reservan memoria (`create`, `add`, `addRow`...): con `-fno-exceptions` casi no mejora el código generado y solo ensucia las firmas.
- **Consecuencias:**
  - El código es más sencillo: no hace falta ordenar las operaciones para deshacerlas ni construir temporales antes de cada mudanza. La `tuple` de temporales de `add` deja de tener sentido y se puede quitar.
  - Un fallo de memoria en release cierra el juego. Se acepta: en un juego no hay una recuperación razonable.
  - Las aserciones de debug son la única red de seguridad: las pruebas del sandbox deben ejecutarse en debug.
  - Los componentes del usuario siguen necesitando un movimiento `noexcept` para cumplir el concepto `Component`.

---

## Fase 3 — Multihilo

### D-10 — Paralelismo: job system y planificación de sistemas
- **Estado / Fase / Fecha:** Abierta · Fase 3
- **Opciones:** paralelismo dentro de un sistema (repartir chunks entre hilos) y/o entre sistemas (grafo de dependencias según qué componentes lee o escribe cada uno); pool de hilos propio con colas de trabajo (*work stealing*) o una sola cola compartida.
- **Decisión:** —
- **Consecuencias:** —
