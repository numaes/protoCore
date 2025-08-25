/*
 * Thread.cpp
 *
 *  Created on: 2020-5-1
 *      Author: gamarino
 */

#include "../headers/proto_internal.h"
#include <cstdlib> // Usar la cabecera C++ estándar
#include <thread>
#include <utility> // Para std::move


namespace proto
{
    // --- Constructor y Destructor ---

    // Constructor modernizado con lista de inicialización y ajustado para no usar plantillas.
    ProtoThreadImplementation::ProtoThreadImplementation(
        ProtoContext* context,
        ProtoString* name,
        ProtoSpace* space,
        ProtoMethod targetCode,
        ProtoList* args,
        ProtoSparseList* kwargs
    ) : Cell(context),
        state(THREAD_STATE_MANAGED),
        name(name),
        space(space),
        osThread(nullptr),
        freeCells(nullptr),
        currentContext(nullptr),
        unmanagedCount(0)
    {
        // Inicializar el cache de métodos
        this->method_cache = std::malloc(THREAD_CACHE_DEPTH * sizeof(*(this->method_cache)));
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            this->method_cache[i].object = nullptr;
            this->method_cache[i].method_name = nullptr;
        }

        // Registrar el hilo en el espacio de memoria.
        this->space->allocThread(context, reinterpret_cast<ProtoThread*>(this));

        // Crear e iniciar el hilo del sistema operativo si se proporciona código para ejecutar.
        if (targetCode)
        {
            // Usar std::unique_ptr para gestionar la vida del hilo de forma segura.
            // La lambda ahora captura por valor para evitar problemas de tiempo de vida.
            this->osThread = new std::thread(
                [=](ProtoThreadImplementation* self)
                {
                    // Cada hilo necesita su propio contexto base.
                    ProtoContext baseContext(nullptr, nullptr, 0,
                                             reinterpret_cast<ProtoThread*>(self), self->space);
                    // Ejecutar el código del hilo.
                    targetCode(
                        &baseContext,
                        self->implAsObject(&baseContext),
                        nullptr,
                        args,
                        kwargs
                    );
                    // Cuando el código termina, el hilo se da de baja.
                    self->space->deallocThread(&baseContext, reinterpret_cast<ProtoThread*>(self));
                },
                this
            );
        }
        else
        {
            // Este es el caso especial para el hilo principal, que no tiene un std::thread
            // asociado porque ya es el hilo principal del proceso.
            // CORRECCIÓN: Se eliminó la creación de un ProtoContext con 'new', que causaba una fuga de memoria.
            // El contexto del hilo principal es gestionado externamente por ProtoSpace.
            this->space->mainThreadId = std::this_thread::get_id();
        }
    }

    // El destructor debe asegurarse de que el hilo del SO se haya unido o separado.
    ProtoThreadImplementation::~ProtoThreadImplementation()
    {
        // Clean up method cache
        std::free(this->method_cache);

        if (this->osThread)
        {
            if (this->osThread->joinable())
            {
                // Por seguridad, si el hilo todavía se puede unir, lo separamos
                // para evitar que el programa termine abruptamente.
                this->osThread->detach();
            }
            delete this->osThread;
            this->osThread = nullptr;
        }
    }

    // --- Métodos de la Interfaz Pública ---

    void ProtoThreadImplementation::implSetUnmanaged()
    {
        this->unmanagedCount++;
        this->state = THREAD_STATE_UNMANAGED;
    }

    void ProtoThreadImplementation::implSetManaged()
    {
        if (this->unmanagedCount > 0)
        {
            this->unmanagedCount--;
        }
        if (this->unmanagedCount == 0)
        {
            this->state = THREAD_STATE_MANAGED;
        }
    }

    void ProtoThreadImplementation::implDetach(ProtoContext* context)
    {
        if (this->osThread && this->osThread->joinable())
        {
            this->osThread->detach();
        }
    }

    void ProtoThreadImplementation::implJoin(ProtoContext* context)
    {
        if (this->osThread && this->osThread->joinable())
        {
            this->osThread->join();
        }
    }

    void ProtoThreadImplementation::implExit(ProtoContext* context)
    {
        // CORRECCIÓN: Añadido un chequeo para evitar un fallo si osThread es nulo (hilo principal).
        if (this->osThread && this->osThread->get_id() == std::this_thread::get_id())
        {
            this->space->deallocThread(context, reinterpret_cast<ProtoThread*>(this));
            // NOTA: En un sistema real, aquí se debería lanzar una excepción o
            // usar un mecanismo para terminar el hilo de forma segura.
            // std::terminate() o similar podría ser una opción, pero es abrupto.
        }
    }

    // --- Sincronización con el Recolector de Basura (GC) ---

    void ProtoThreadImplementation::implSynchToGC()
    {
        // CORRECCIÓN CRÍTICA: La lógica de estado estaba rota.
        // Se debe comprobar el estado del 'space', no el del 'thread'.
        if (this->state == THREAD_STATE_MANAGED && this->space->state != SPACE_STATE_RUNNING)
        {
            if (this->space->state == SPACE_STATE_STOPPING_WORLD)
            {
                this->state = THREAD_STATE_STOPPING;
                this->space->stopTheWorldCV.notify_one();

                // Esperar a que el GC indique que el mundo debe detenerse.
                std::unique_lock lk(ProtoSpace::globalMutex);
                this->space->restartTheWorldCV.wait(lk, [this]
                {
                    return this->space->state == SPACE_STATE_WORLD_TO_STOP;
                });

                this->state = THREAD_STATE_STOPPED;
                this->space->stopTheWorldCV.notify_one();

                // Esperar a que el GC termine y el mundo se reinicie.
                this->space->restartTheWorldCV.wait(lk, [this]
                {
                    return this->space->state == SPACE_STATE_RUNNING;
                });

                this->state = THREAD_STATE_MANAGED;
            }
        }
    }

    Cell* ProtoThreadImplementation::implAllocCell()
    {
        if (!this->freeCells)
        {
            // Si nos quedamos sin celdas locales, sincronizamos con el GC
            // y pedimos un nuevo bloque de celdas al espacio.
            this->implSynchToGC();
            this->freeCells = static_cast<BigCell*>(this->space->getFreeCells(reinterpret_cast<ProtoThread*>(this)));
        }

        // Tomar la primera celda de la lista local.
        Cell* newCell = this->freeCells;
        if (newCell)
        {
            this->freeCells = static_cast<BigCell*>(newCell->nextCell);
            newCell->nextCell = nullptr; // Desvincularla completamente.
        }

        return newCell;
    }

    // --- Métodos del Recolector de Basura (GC) ---

    void ProtoThreadImplementation::finalize(ProtoContext* context)
    {
    };

    void ProtoThreadImplementation::processReferences(
        ProtoContext* context,
        void* self,
        void (*method)(ProtoContext* context, void* self, Cell* cell)
    )
    {
        // 1. El nombre del hilo. Aunque los ProtoString son inmortales debido a la
        //    internalización, es una buena práctica tratarlo como una raíz por completitud.
        if (this->name && this->name->isCell(context))
        {
            method(context, self, this->name->asCell(context));
        }

        // 2. El caché de métodos. Es VITAL escanear los punteros a OBJETOS.
        //    Si un objeto sale del scope pero permanece en el caché, esta es la
        //    única referencia que lo mantiene vivo, previniendo un bug de 'use-after-free'.
        //    NOTA: No es necesario escanear 'method_name' porque los ProtoString son
        //    inmortales (internalizados en 'tupleRoot') y nunca serán recolectados.
        for (unsigned int i = 0; i < THREAD_CACHE_DEPTH; ++i)
        {
            if (this->method_cache[i].object)
            {
                method(context, self, this->method_cache[i].object->asCell(context));
            }
        }

        // 2. La cadena de contextos (el stack de llamadas del hilo).
        ProtoContext* ctx = this->currentContext;
        while (ctx)
        {
            // 3. Las variables locales en cada frame del stack.
            if (ctx->localsBase)
            {
                for (unsigned int i = 0; i < ctx->localsCount; ++i)
                {
                    if (ctx->localsBase[i] && ctx->localsBase[i]->isCell(context))
                    {
                        method(context, self, ctx->localsBase[i]->asCell(context));
                    }
                }
            }
            ctx = ctx->previous;
        }

        // 4. La lista de celdas libres locales del hilo.
        Cell* currentFree = this->freeCells;
        while (currentFree)
        {
            method(context, self, currentFree);
            currentFree = currentFree->nextCell;
        }
    }

    ProtoObject* ProtoThreadImplementation::implAsObject(ProtoContext* context)
    {
        ProtoObjectPointer p;
        p.oid.oid = (ProtoObject*)this;
        // CORRECCIÓN CRÍTICA: Usar el tag correcto para un hilo.
        p.op.pointer_tag = POINTER_TAG_THREAD;
        return p.oid.oid;
    }

    void ProtoThreadImplementation::implSetCurrentContext(ProtoContext* context)
    {
        this->currentContext = context;
    }

    ProtoContext* ProtoThreadImplementation::implGetCurrentContext()
    {
        return this->currentContext;
    }

    unsigned long ProtoThreadImplementation::getHash(ProtoContext* context)
    {
        // El hash de una Cell se deriva directamente de su dirección de memoria.
        // Esto proporciona un identificador rápido y único para el objeto.
        ProtoObjectPointer p{};
        p.oid.oid = reinterpret_cast<ProtoObject*>(this);

        return p.asHash.hash;
    }

    ProtoThread* ProtoThreadImplementation::implGetCurrentThread(ProtoContext* context)
    {
        return context->thread;
    }
} // namespace proto
