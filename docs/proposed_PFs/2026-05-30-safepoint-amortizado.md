# Propuesta de Proyecto Final

> **Material de trabajo** para conversación inicial con ITBA sobre líneas
> de PFs en el área de runtimes / lenguajes. Borrador, no formalizado.
> Conservado en español por ser insumo para conversación en contexto
> hispanoparlante; si se formaliza para el repositorio principal, se
> traducirá según la convención del proyecto.
>
> **Fecha:** 2026-05-30
> **Propuesto sobre:** `NEXT_STEPS.md` § 1 (GC quorum latency reduction).

---

## Título

**"Mecanismo de Safepoint Cooperativo Amortizado para Garbage Collector
Concurrente"**

## Contexto

`protoCore` es un runtime open-source de objetos dinámicos que implementa
un garbage collector concurrente mark-and-sweep con pausas STW
sub-milisegundo. Una reciente reestructuración (mayo 2026) movió la fase
de mark fuera de la ventana STW mediante un mecanismo de snapshot-at-STW
de los shards mutables, eliminando barreras de escritura sin sacrificar
corrección.

Tras esa mejora, el costo dominante restante del STW es el **quorum
wait**: el tiempo entre que el GC solicita la suspensión cooperativa y
el último thread de usuario llega a un safepoint y la reconoce. Este
tiempo no está acotado en la implementación actual — un thread
ejecutando un loop computacional sin allocations puede demorar la GC
indefinidamente.

El presente PF propone diseñar, implementar y evaluar empíricamente un
mecanismo de safepoint cooperativo amortizado por contador thread-local,
integrado al callback de línea de debug que los frontends ya emiten al
runtime.

## Objetivos específicos

1. Diseñar la API y contrato del mecanismo (entrypoints
   `lineCheckpoint(int)` y `safepointTick()` en `ProtoContext`).
2. Implementar el mecanismo en `protoCore/core/ProtoContext.{h,cpp}` con
   el contador thread-local y la slow path de handshake con el GC.
3. Instrumentar `protoCore` con métricas per-cycle de quorum wait
   (histograma per-thread, per-cycle, exportable).
4. Integrar el mecanismo en al menos un frontend existente (sugerido:
   `protoPython`, por estabilidad y suite de benchmarks completa).
5. Evaluar empíricamente el impacto del mecanismo en:
   - Reducción de worst-case quorum latency.
   - Overhead introducido en el hot path del frontend (degradación
     porcentual de benchmarks vs baseline sin el mecanismo).
   - Sensibilidad al parámetro `SAFEPOINT_INTERVAL` (barrido de valores
     256, 1024, 4096, 16384).
6. Producir un informe técnico con resultados, recomendaciones de
   configuración, y comparación con safepoint mechanisms análogos en
   HotSpot, V8 y Go.

## Alcance

**Incluido:**

- Diseño formal y especificación de la API.
- Implementación en `protoCore`.
- Instrumentación y suite de medición.
- Integración con un frontend (`protoPython` recomendado).
- Evaluación empírica en benchmarks existentes (mt100k, suite de
  `protoPython`).
- Análisis comparativo con literatura existente.

**No incluido** (queda para PFs futuros):

- Integración en más de un frontend.
- Optimizaciones tipo page-protect trick para safepoint poll.
- Decentralized root scan.
- Cambios al compilador de `protoPyC`.

## Metodología y cronograma

| Fase | Trabajo | Duración |
|------|---------|----------|
| 1. Diseño y especificación | Análisis de la base de código, especificación formal del contrato, definición de API, revisión con director | 3-4 semanas |
| 2. Implementación | C++20 siguiendo estándares del proyecto, tests unitarios | 4-6 semanas |
| 3. Instrumentación | Agregar métricas sin alterar performance del path no-instrumentado | 2-3 semanas |
| 4. Integración | Con `protoPython`, asegurar que toda la test suite sigue pasando | 3-4 semanas |
| 5. Evaluación empírica | Benchmarks, recolección de métricas, análisis, ajuste de configuración | 4-6 semanas |
| 6. Documentación e informe | Informe final con eventual vista a publicación | 3-4 semanas |

**Total:** 19-27 semanas (compatible con PF intensivo de un cuatrimestre
o relajado a dos cuatrimestres).

## Criterios de éxito mensurables

- Mecanismo implementado y compilando sin warnings bajo
  `-Wall -Wextra -Wpedantic`.
- Toda la test suite existente de `protoCore` y `protoPython` pasa sin
  regresiones.
- Worst-case quorum latency reducido al menos en factor 10× para el
  peor benchmark identificado en el baseline.
- Overhead en el hot path inferior al 2% en los benchmarks de
  `protoPython`.
- Informe técnico con datos cuantitativos y comparación contra al menos
  tres sistemas análogos.

## Prerrequisitos del estudiante

**Requeridos:**

- Sólida base en C++ moderno (C++17+).
- Comprensión de programación concurrente y modelos de memoria.
- Familiaridad básica con conceptos de garbage collection
  (mark-and-sweep, generational, concurrent).
- Capacidad de leer código existente y razonar sobre invariantes
  implícitas.

**No requeridos** (se adquieren durante el proyecto):

- Experiencia previa en implementación de runtimes.
- Conocimiento de Python a nivel runtime (suficiente con Python a nivel
  aplicación).

## Entregables

1. Código fuente integrado al repositorio `protoCore` (commits con
   autoría del estudiante, revisados por director del proyecto).
2. Suite de tests unitarios cubriendo el mecanismo.
3. Suite de benchmarks de medición, reproducible.
4. Dataset de resultados experimentales (formato reproducible).
5. Informe técnico (~30-50 páginas): introducción, marco teórico,
   diseño, implementación, evaluación, conclusiones.
6. Presentación de defensa.
7. **Opcional:** paper corto (~6-8 páginas) submittable a un workshop
   académico (ISMM, PLDI MGS workshop, similar).

## Valor para el estudiante

Exposición a:

- Implementación real de un mecanismo descripto en literatura de
  runtimes serios (HotSpot, V8, Go).
- Programación lock-free con C++ atomics.
- Diseño de APIs internas con contratos no triviales.
- Evaluación empírica rigurosa (no demostración por ejemplo).
- Lectura de literatura técnica primaria.

## Valor para `protoCore` y para ITBA

**Para `protoCore`:** un mecanismo completo, medido e integrado, listo
para uso en producción. Resuelve un problema arquitectónico real, no
académico.

**Para ITBA:** un PF con contribución concreta a un proyecto
open-source serio, producción publicable inmediata, y validación del
grupo como capaz de hacer trabajo de runtimes a nivel internacional.
Es exactamente el tipo de trabajo que diferencia "centro que enseña
compiladores" de "centro que investiga runtimes".

## Posibles PFs subsiguientes

Este proyecto es deliberadamente el primero de una línea. Cada uno de
los siguientes es un PF independiente compatible con el primero, y
juntos conforman un programa de investigación sostenido:

- Implementación del mismo mecanismo en `protoJS` y `protoST`
  (verificación de portabilidad del contrato entre frontends).
- Decentralized root scan (cf. `NEXT_STEPS.md` § 2c).
- Parallel mark phase (cf. `NEXT_STEPS.md` § 2b).
- Page-protect trick para safepoint poll
  (cf. `NEXT_STEPS.md` § 1, deferred).
- Soft real-time framework de medición completo
  (cf. `NEXT_STEPS.md` § 2a).
- Binding básico a un lenguaje alternativo (ej. Nim o Roc)
  (cf. `NEXT_STEPS.md` § 4).
- Wrappers ergonómicos para uso de `protoCore` como librería desde
  C++ aplicación (cf. `NEXT_STEPS.md` § 3).

La existencia documentada de esta lista demuestra que **no es un PF
aislado, sino el inicio de una línea de investigación con material
para 3-5 años de PFs y eventuales tesis de maestría**.

## Referencias internas

- `docs/GarbageCollector.md` — diseño actual del GC.
- `docs/STW_ELIMINATION_RESEARCH.md` — historia y razonamiento de la
  reducción de pausas STW, en particular § 13.
- `docs/NEXT_STEPS.md` § 1 — diseño base de este PF.
- `docs/NEXT_STEPS.md` § 2 — catálogo de PFs subsiguientes posibles.

## Referencias externas iniciales (lectura previa sugerida)

- *The Garbage Collection Handbook* (Jones, Hosking, Moss, 2nd ed.) —
  capítulos sobre concurrent collection y safepoint mechanisms.
- HotSpot VM safepoint documentation (OpenJDK source, comments en
  `safepoint.cpp`).
- Go GC design documents (1.5 onwards, particularmente Go 1.14 async
  preemption).
- Click, Tene & Wolf, "The Pauseless GC Algorithm" (Azul C4) — para
  contraste arquitectónico.
