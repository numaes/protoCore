# Hoja de Ruta y Próximos Pasos para Proto

Este documento resume las decisiones clave, las correcciones críticas y la hoja de ruta estratégica para la evolución del proyecto Proto, basado en un análisis exhaustivo de su arquitectura.

## Resumen Estratégico

Proto no es un competidor generalista de runtimes como Python o V8. Su verdadera fortaleza y posicionamiento en el mercado es el de una **fundación de runtime de C++ de alto rendimiento y baja latencia**.

*   **Identidad:** Un motor para sistemas de próxima generación.
*   **Público Objetivo:** Motores de videojuegos, sistemas financieros (Fintech), sistemas embebidos/tiempo real y creadores de Lenguajes de Dominio Específico (DSL).
*   **Ventajas Competitivas:**
    1.  **GC de Latencia Ultra-Baja:** Un recolector de basura concurrente con pausas "stop-the-world" mínimas, ideal para aplicaciones sensibles a la latencia.
    2.  **Modelo de Mutabilidad Lock-Free:** Un sistema innovador que separa la identidad del estado, simplificando el escaneo de raíces del GC y permitiendo concurrencia de alto rendimiento.
    3.  **Fundación Agnóstica:** Es una biblioteca, no un lenguaje, lo que permite construir cualquier estrategia de ejecución (intérprete, VM, compilador AOT) sobre ella.
    4.  **FFI Simplificada:** El GC no mueve la memoria, lo que hace que la integración con código C/C++ sea inherentemente más simple y segura.

---

## 1. Correcciones Críticas de Robustez

Estas son correcciones indispensables para garantizar la estabilidad y correctitud del runtime.

### 1.1. (CRÍTICO) Escaneo del Caché de Métodos por el GC

**Problema:** El `method_cache` en `ProtoThreadImplementation` almacena punteros a objetos (`ProtoObject*`). Si un objeto sale del scope del stack pero permanece en el caché, el GC lo liberará prematuramente, creando un puntero colgante. Esto conduce a un bug de `use-after-free` cuando la memoria es reutilizada y se produce un "falso positivo" en el caché.

**Decisión:** El caché de métodos **debe** ser tratado como una fuente de raíces para el GC. La función `processReferences` del hilo debe iterar sobre el caché y reportar los punteros a `object` que contiene. No es necesario escanear `method_name` ya que los `ProtoString` son inmortales (internalizados).

**Implementación (`Thread.cpp`):**
```diff
--- a/home/gamarino/Documentos/proyectos/proto/proto/core/Thread.cpp
+++ b/home/gamarino/Documentos/proyectos/proto/proto/core/Thread.cpp
@@ -236,16 +236,25 @@
         void (*method)(ProtoContext* context, void* self, Cell* cell)
     )
     {
-        // 1. El nombre del hilo (si es una string gestionada).
-        // TODO REVISAR
-        // if (this->name && this->name->isCell(context)) {
-        //     method(context, self, this->name->asCell(context));
-        //}
+        // 1. El nombre del hilo. Aunque los ProtoString son inmortales debido a la
+        //    internalización, es una buena práctica tratarlo como una raíz por completitud.
+        if (this->name && this->name->isCell(context))
+        {
+            method(context, self, this->name->asCell(context));
+        }
+
+        // 2. El caché de métodos. Es VITAL escanear los punteros a OBJETOS.
+        //    Si un objeto sale del scope pero permanece en el caché, esta es la
+        //    única referencia que lo mantiene vivo, previniendo un bug de 'use-after-free'.
+        //    NOTA: No es necesario escanear 'method_name' porque los ProtoString son
+        //    inmortales (internalizados en 'tupleRoot') y nunca serán recolectados.
+        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
+        {
+            if (this->method_cache[i].object)
+            {
+                method(context, self, this->method_cache[i].object->asCell(context));
+            }
+        }
 
         // 2. La cadena de contextos (el stack de llamadas del hilo).
         ProtoContext* ctx = this->currentContext;

```

### 1.2. Corrección del Hash en el Caché de Métodos

**Problema:** El cálculo del hash en `ProtoObject::call` usaba `reinterpret_cast<int>`, lo que trunca los punteros en arquitecturas de 64 bits, causando colisiones excesivas y degradando el rendimiento del caché.

**Decisión:** Usar `uintptr_t` para la conversión de puntero a entero, garantizando que se utilice la dirección completa del puntero sin pérdida de datos.

**Implementación (`Proto.cpp`):**
```diff
--- a/home/gamarino/Documentos/proyectos/proto/proto/core/Proto.cpp
+++ b/home/gamarino/Documentos/proyectos/proto/proto/core/Proto.cpp
@@ -223,7 +223,8 @@
     {
         auto thread = reinterpret_cast<ProtoThreadImplementation>(c->thread);
 
-        unsigned int hash = (reinterpret_cast<int>(this) ^ reinterpret_cast<int>(method)) & (THREAD_CACHE_DEPTH - 1);
+        // Usar uintptr_t para un hash correcto y portable en 64 bits.
+        uintptr_t full_hash = (reinterpret_cast<uintptr_t>(this) ^ reinterpret_cast<uintptr_t>(method));
+        unsigned int hash = full_hash & (THREAD_CACHE_DEPTH - 1);
         if (thread->method_cache[hash].object != this || thread->method_cache[hash].method_name != method)
         {
             thread->method_cache[hash].object = this;

```

---

## 2. Modernización y Profesionalización del Código

Estos cambios mejoran la seguridad, legibilidad y mantenimiento del proyecto, alineándolo con las prácticas modernas de C++.

1.  **Gestión de Hilos con RAII:** Reemplazar `new std::thread` y `delete` en `ProtoThreadImplementation` por `std::unique_ptr<std::thread>`. Esto previene fugas de memoria y simplifica el código.
2.  **Asignación de Arrays Idiomática:** Reemplazar `std::malloc`/`std::free` para el `method_cache` por `new[]`/`delete[]`.
3.  **Sistema de Compilación Profesional:** Migrar el `Makefile` a **CMake**. Esto es crucial para la accesibilidad multiplataforma y para que otros colaboradores puedan compilar el proyecto fácilmente en Linux, macOS y Windows.

---

## 3. Hoja de Ruta Estratégica

### Fase 1: MVP para Presentación Universitaria (Comienzos del próximo año)

**Objetivo:** Demostrar el concepto y el potencial de Proto de forma impactante.

*   **[ ] Implementar las correcciones críticas (Sección 1).**
*   **[ ] `proto_python` (Transpilador):**
    *   Soportar un subconjunto funcional de Python: asignación, operaciones aritméticas, definición y llamada de funciones, bucles.
    *   Usar el módulo `ast` de Python como front-end.
*   **[ ] REPL (Read-Eval-Print Loop):** Crear una consola interactiva para `proto_python`. Es la herramienta de demo más efectiva.
*   **[ ] Benchmark `protoDB`:**
    *   Identificar 1-2 funciones cuello de botella en `protoDB` (ej. búsqueda ANN).
    *   Transpilar **solo esas funciones** con `proto_python`.
    *   Crear un benchmark que compare la versión pura de Python con la versión híbrida (Python llamando a la función compilada con Proto) para mostrar una diferencia de velocidad drástica.

### Fase 2: MVP para Lanzamiento Internacional (Mitad del próximo año)

**Objetivo:** Posicionar a Proto como un proyecto de código abierto serio y atraer colaboradores.

*   **[ ] Profesionalizar el Proyecto:**
    *   Implementar un sistema de compilación con **CMake**.
    *   Crear una suite de **pruebas unitarias** sólida para el runtime.
*   **[ ] Documentación de Clase Mundial (en Inglés):**
    *   **README.md:** Profesional y con una guía de inicio rápido.
    *   **Documentación de Arquitectura:** Crear una carpeta `/docs` o un Wiki en GitHub con "whitepapers" sobre el GC, el modelo de mutabilidad, etc.
    *   **Documentación de API:** Usar **Doxygen** para generar una referencia de la API a partir de comentarios en el código.
*   **[ ] Guías para la Comunidad:**
    *   Crear un archivo `CONTRIBUTING.md`.
    *   Identificar y etiquetar issues como `good first issue` en GitHub.

### Fase 3: Visión a Largo Plazo (Construcción del Ecosistema)

*   **[ ] Biblioteca Estándar:** Construir módulos básicos (archivos, matemáticas, etc.) usando la FFI de Proto.
*   **[ ] Herramientas de Depuración:** Añadir "hooks" al runtime para permitir un depurador paso a paso y un visualizador del estado del GC.
*   **[ ] Gestor de Paquetes:** Diseñar un sistema simple para que la comunidad pueda compartir bibliotecas.

---

## 4. Plan de Divulgación y Comunidad

1.  **Crear Contenido Técnico (Blog):** Escribir artículos en plataformas como Medium o dev.to sobre los aspectos únicos de Proto.
    *   *Ejemplo: "Cómo construí un Recolector de Basura de Baja Latencia en C++".*
    *   *Ejemplo: "Transpilando Python a C++: Un Caso de Estudio con `proto_python`".*
2.  **Publicación Académica:** Publicar un "whitepaper" en **arXiv.org** (categoría `cs.PL`) para darle credibilidad y visibilidad académica internacional.
3.  **Divulgación en Comunidades Online:** Compartir los artículos del blog y las demos en:
    *   **Hacker News** (con el tag `Show HN:`).
    *   **Reddit** (en `/r/cpp`, `/r/compilers`, `/r/ProgrammingLanguages`).
4.  **Demos en Video:** Crear un video corto mostrando el benchmark de `protoDB` lado a lado. **Ver la velocidad es más poderoso que leer sobre ella.**