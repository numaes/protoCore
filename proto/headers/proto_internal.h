/* 
 * proto_internal.h
 *
 *  Created on: November, 2017 - Redesign January, 2024
 *      Author: Gustavo Adrian Marino <gamarino@numaes.com>
 */


#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include "../headers/proto.h"
#include <thread>

// Method cache per thread. It should be a power of 2
#define THREAD_CACHE_DEPTH 1024

namespace proto
{
    // Forward declarations for implementation classes
    class Cell;
    class BigCell;
    class ProtoObjectCellImplementation;
    class ParentLinkImplementation;
    class ProtoListImplementation;
    class ProtoSparseListImplementation;
    class ProtoTupleImplementation;
    class ProtoStringImplementation;
    class ProtoMethodCellImplementation;
    class ProtoExternalPointerImplementation;
    class ProtoByteBufferImplementation;
    class ProtoThreadImplementation;

    // Union for pointer tagging
    union ProtoObjectPointer
    {
        struct
        {
            ProtoObject* oid;
        } oid;

        struct
        {
            ProtoObjectCellImplementation* objectCell;
        } oc;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long value : 56;
        } op;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            long smallInteger : 56;
        } si;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long floatValue : 32;
        } sd;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long unicodeValue : 56;
        } unicodeChar;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long booleanValue : 1;
            unsigned long padding : 55;
        } booleanValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned char byteData;
        } byteValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long year : 16;
            unsigned long month : 8;
            unsigned long day : 8;
            unsigned long padding : 24;
        } date;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            unsigned long timestamp : 56;
        } timestampValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long embedded_type : 4;
            long timedelta : 56;
        } timedeltaValue;

        struct
        {
            unsigned long pointer_tag : 4;
            unsigned long hash : 60;
        } asHash;

        struct
        {
            unsigned long pointer_tag : 4;
            Cell* cell;
        } cell;
    };

    class AllocatedSegment
    {
    public:
        BigCell* memoryBlock;
        int cellsCount;
        AllocatedSegment* nextBlock;
    };

    class DirtySegment
    {
    public:
        BigCell* cellChain;
        DirtySegment* nextSegment;
    };

    // Pointer tags
#define POINTER_TAG_OBJECT                  0
#define POINTER_TAG_EMBEDEDVALUE            1
#define POINTER_TAG_LIST                    2
#define POINTER_TAG_LIST_ITERATOR           3
#define POINTER_TAG_TUPLE                   4
#define POINTER_TAG_TUPLE_ITERATOR          5
#define POINTER_TAG_STRING                  6
#define POINTER_TAG_STRING_ITERATOR         7
#define POINTER_TAG_SPARSE_LIST             8
#define POINTER_TAG_SPARSE_LIST_ITERATOR    9
#define POINTER_TAG_BYTE_BUFFER             10
#define POINTER_TAG_EXTERNAL_POINTER        11
#define POINTER_TAG_METHOD                  12
#define POINTER_TAG_THREAD                  13

    // Embedded types
#define EMBEDED_TYPE_SMALLINT               0
#define EMBEDED_TYPE_FLOAT                  1
#define EMBEDED_TYPE_UNICODECHAR            2
#define EMBEDED_TYPE_BOOLEAN                3
#define EMBEDED_TYPE_BYTE                   4
#define EMBEDED_TYPE_DATE                   5
#define EMBEDED_TYPE_TIMESTAMP              6
#define EMBEDED_TYPE_TIMEDELTA              7

    // Iterator states
#define ITERATOR_NEXT_PREVIOUS              0
#define ITERATOR_NEXT_THIS                  1
#define ITERATOR_NEXT_NEXT                  2

    // Space states
#define SPACE_STATE_RUNNING                 0
#define SPACE_STATE_STOPPING_WORLD          1
#define SPACE_STATE_WORLD_TO_STOP           2
#define SPACE_STATE_WORLD_STOPPED           3
#define SPACE_STATE_ENDING                  4

    // Thread states
#define THREAD_STATE_UNMANAGED              0
#define THREAD_STATE_MANAGED                1
#define THREAD_STATE_STOPPING               2
#define THREAD_STATE_STOPPED                3
#define THREAD_STATE_ENDED                  4

#define TYPE_SHIFT                          4

    // Plantilla para convertir de puntero a la API pública a puntero a la implementación
    template <typename Impl, typename Api>
    inline Impl* toImpl(Api* ptr)
    {
        return reinterpret_cast<Impl*>(ptr);
    }

    // Plantilla para convertir de puntero a la API pública constante
    template <typename Impl, typename Api>
    inline const Impl* toImpl(const Api* ptr)
    {
        return reinterpret_cast<const Impl*>(ptr);
    }

    unsigned long generate_mutable_ref();

    class Cell
    {
    public:
        static void* operator new(unsigned long size, ProtoContext* context);

        explicit Cell(ProtoContext* context);
        virtual ~Cell();

        virtual void finalize(ProtoContext* context);
        virtual void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        virtual unsigned long getHash(ProtoContext* context);
        virtual ProtoObject* asObject(ProtoContext* context);

        Cell* nextCell;
    };

    class BigCell : public Cell
    {
    public:
        BigCell(ProtoContext* context);
        ~BigCell();

        void finalize(ProtoContext* context)
        {
            /* No special finalization needed for BigCell */
        };

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        )
        {
            /* BigCell does not hold references to other Cells */
        };
        unsigned long getHash(ProtoContext* context) { return Cell::getHash(context); };
        ProtoObject* asObject(ProtoContext* context) { return Cell::asObject(context); };

        void * undetermined[6];
    };

    static_assert(sizeof(BigCell) == 64, "El tamaño de la clase BigCell debe ser exactamente 64 bytes.");


    class ParentLinkImplementation : public Cell, public ParentLink
    {
    public:
        explicit ParentLinkImplementation(
            ProtoContext* context,
            ParentLinkImplementation* parent,
            ProtoObjectCellImplementation* object
        );
        ~ParentLinkImplementation();

        ProtoObject* asObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        void finalize(ProtoContext* context);

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        ParentLinkImplementation* parent;
        ProtoObjectCellImplementation* object;
    };

    /**
     * @class ProtoObjectCellImplementation
     * @brief Implementación de una celda que representa un objeto Proto.
     *
     * Hereda de 'Cell' para la gestión de memoria y de 'ProtoObjectCell'
     * para la interfaz pública de objetos. Contiene una referencia a su cadena
     * de herencia (padres) y una lista de atributos.
     */
    class ProtoObjectCellImplementation : public Cell, public ProtoObject
    {
    public:
        /**
         * @brief Constructor.
         * @param context El contexto de ejecución actual.
         * @param parent Puntero al primer eslabón de la cadena de herencia.
         * @param mutable_ref Un indicador de si el objeto es mutable.
         * @param attributes La lista dispersa de atributos del objeto.
         */
        ProtoObjectCellImplementation(
            ProtoContext* context,
            ParentLinkImplementation* parent,
            unsigned long mutable_ref,
            ProtoSparseListImplementation* attributes
        );

        /**
         * @brief Destructor virtual.
         */
        ~ProtoObjectCellImplementation();

        /**
         * @brief Crea una nueva celda de objeto con un padre adicional en su cadena de herencia.
         * @param context El contexto de ejecución actual.
         * @param newParentToAdd El objeto padre que se va a añadir.
         * @return Una *nueva* ProtoObjectCellImplementation con la cadena de herencia actualizada.
         */
        ProtoObjectCell* implAddParent(
            ProtoContext* context,
            ProtoObjectCell* newParentToAdd
        );

        /**
         * @brief Devuelve la representación de esta celda como un ProtoObject.
         * @param context El contexto de ejecución actual.
         * @return Un ProtoObject que representa este objeto.
         */
        ProtoObject* implAsObject(ProtoContext* context);

        /**
         * @brief Finalizador para el recolector de basura.
         *
         * Este método es llamado por el GC antes de liberar la memoria de la celda.
         * @param context El contexto de ejecución actual.
         */
        void finalize(ProtoContext* context);

        /**
         * @brief Procesa las referencias internas para el recolector de basura.
         *
         * Recorre las referencias al padre y a los atributos para que el GC
         * pueda marcar los objetos alcanzables.
         */
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell)
        );
        long unsigned getHash(ProtoContext* context);

        ParentLinkImplementation* parent;
        unsigned long mutable_ref;
        ProtoSparseListImplementation* attributes;
    };

    // --- ProtoListIterator ---
    // Concrete implementation for ProtoObject*
    class ProtoListIteratorImplementation : public Cell, public ProtoListIterator
    {
    public:
        ProtoListIteratorImplementation(
            ProtoContext* context,
            ProtoListImplementation* base,
            unsigned long currentIndex
        );
        ~ProtoListIteratorImplementation();

        int implHasNext(ProtoContext* context);
        ProtoObject* implNext(ProtoContext* context);
        ProtoListIteratorImplementation* implAdvance(ProtoContext* context);

        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        void finalize(ProtoContext* context);

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

    private:
        ProtoListImplementation* base;
        unsigned long currentIndex;
    };

    // --- ProtoList ---
    // Concrete implementation for ProtoObject*
    class ProtoListImplementation : public Cell, public ProtoList
    {
    public:
        explicit ProtoListImplementation(
            ProtoContext* context,
            ProtoObject* value = PROTO_NONE,
            ProtoListImplementation* previous = nullptr,
            ProtoListImplementation* next = nullptr
        );
        ~ProtoListImplementation();

        ProtoObject* implGetAt(ProtoContext* context, int index);
        ProtoObject* implGetFirst(ProtoContext* context);
        ProtoObject* implGetLast(ProtoContext* context);
        ProtoListImplementation* implGetSlice(ProtoContext* context, int from, int to);
        unsigned long implGetSize(ProtoContext* context);

        bool implHas(ProtoContext* context, ProtoObject* value);
        ProtoListImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value = PROTO_NONE);
        ProtoListImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value);

        ProtoListImplementation* implAppendFirst(ProtoContext* context, ProtoObject* value);
        ProtoListImplementation* implAppendLast(ProtoContext* context, ProtoObject* value);

        ProtoListImplementation* implExtend(ProtoContext* context, ProtoList* other);

        ProtoListImplementation* implSplitFirst(ProtoContext* context, int index);
        ProtoListImplementation* implSplitLast(ProtoContext* context, int index);

        ProtoListImplementation* implRemoveFirst(ProtoContext* context);
        ProtoListImplementation* implRemoveLast(ProtoContext* context);
        ProtoListImplementation* implRemoveAt(ProtoContext* context, int index);
        ProtoListImplementation* implRemoveSlice(ProtoContext* context, int from, int to);

        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        ProtoListIteratorImplementation* implGetIterator(ProtoContext* context);

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        ProtoListImplementation* previous;
        ProtoListImplementation* next;

        ProtoObject* value;
        unsigned long hash{};

        unsigned long count : 52{};
        unsigned long height : 8;
        unsigned long type : 4{};
    };

    // --- ProtoSparseList ---
    // Concrete implementation for ProtoObject*
    class ProtoSparseListIteratorImplementation : public Cell, public ProtoSparseListIterator
    {
    public:
        ProtoSparseListIteratorImplementation(
            ProtoContext* context,
            int state,
            ProtoSparseListImplementation* current,
            ProtoSparseListIteratorImplementation* queue = NULL
        );
        ~ProtoSparseListIteratorImplementation();

        int implHasNext(ProtoContext* context);
        unsigned long implNextKey(ProtoContext* context);
        ProtoObject* implNextValue(ProtoContext* context);

        ProtoSparseListIteratorImplementation* implAdvance(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

    private:
        int state;
        ProtoSparseListImplementation* current;
        ProtoSparseListIteratorImplementation* queue;
    };

    class ProtoSparseListImplementation : public Cell, public ProtoSparseList
    {
    public:
        explicit ProtoSparseListImplementation(
            ProtoContext* context,
            unsigned long index = 0,
            ProtoObject* value = PROTO_NONE,
            ProtoSparseListImplementation* previous = NULL,
            ProtoSparseListImplementation* next = NULL
        );
        ~ProtoSparseListImplementation();

        bool implHas(ProtoContext* context, unsigned long index);
        ProtoObject* implGetAt(ProtoContext* context, unsigned long index);
        ProtoSparseListImplementation* implSetAt(ProtoContext* context, unsigned long index, ProtoObject* value);
        ProtoSparseListImplementation* implRemoveAt(ProtoContext* context, unsigned long index);
        int implIsEqual(ProtoContext* context, ProtoSparseList* otherDict);
        ProtoObject* implGetAtOffset(ProtoContext* context, int offset);

        unsigned long implGetSize(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        virtual ProtoSparseListIteratorImplementation* implGetIterator(ProtoContext* context);

        void implProcessElements(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                unsigned long index,
                ProtoObject* value
            )
        );

        void implProcessValues(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                ProtoObject* value
            )
        );

        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        ProtoSparseListImplementation* previous;
        ProtoSparseListImplementation* next;

        unsigned long index;
        ProtoObject* value;
        unsigned long hash;

        unsigned long count : 52;
        unsigned long height : 8;
        unsigned long type : 4{};
    };

    // --- Iterador de Tuplas ---
#define TUPLE_SIZE 5

    class TupleDictionary : public Cell
    {
    public:
        TupleDictionary* next;
        TupleDictionary* previous;
        ProtoTupleImplementation* key;
        int count;
        int height;

        int compareTuple(ProtoContext* context, ProtoTuple* tuple);
        TupleDictionary* rightRotate(ProtoContext* context);
        TupleDictionary* leftRotate(ProtoContext* context);
        TupleDictionary* rebalance(ProtoContext* context);

        TupleDictionary(
            ProtoContext* context,
            ProtoTupleImplementation* key = nullptr,
            TupleDictionary* next = nullptr,
            TupleDictionary* previous = nullptr
        );

        long unsigned int getHash(proto::ProtoContext*);
        ProtoObject* asObject(proto::ProtoContext*);
        void finalize(ProtoContext* context);

        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(
                ProtoContext* context,
                void* self,
                Cell* cell
            )
        );

        int compareList(ProtoContext* context, ProtoList* list);
        bool hasList(ProtoContext* context, ProtoList* list);
        bool has(ProtoContext* context, ProtoTuple* tuple);
        ProtoTupleImplementation* getAt(ProtoContext* context, ProtoTupleImplementation* tuple);
        TupleDictionary* set(ProtoContext* context, ProtoTupleImplementation* tuple);
    };

    // Implementación concreta para ProtoTupleIterator
    class ProtoTupleIteratorImplementation : public Cell, public ProtoTupleIterator
    {
    public:
        // Constructor
        ProtoTupleIteratorImplementation(
            ProtoContext* context,
            ProtoTupleImplementation* base,
            unsigned long currentIndex
        );

        // Destructor
        ~ProtoTupleIteratorImplementation();

        // --- Métodos de la interfaz ProtoTupleIterator ---
        int implHasNext(ProtoContext* context);
        ProtoObject* implNext(ProtoContext* context);
        ProtoTupleIteratorImplementation* implAdvance(ProtoContext* context);

        // --- Métodos de la interfaz Cell ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        ProtoTupleImplementation* base; // La tupla que se está iterando
        unsigned long currentIndex; // La posición actual en la tupla
    };

    // --- ProtoTuple ---
    // Implementación de tuplas, potencialmente usando una estructura de "rope" para eficiencia.
    class ProtoTupleImplementation : public Cell, public ProtoTuple
    {
    public:
        // Constructor
        ProtoTupleImplementation(
            ProtoContext* context,
            unsigned long elementCount,
            unsigned long heigh,
            ProtoObject** data
        );

        ProtoTupleImplementation(
            ProtoContext* context,
            unsigned long elementCount,
            unsigned long height,
            ProtoTupleImplementation** indirect
        );

        // Destructor
        ~ProtoTupleImplementation();

        // --- Métodos de la interfaz ProtoTuple ---
        ProtoObject* implGetAt(ProtoContext* context, int index);
        ProtoObject* implGetFirst(ProtoContext* context);
        ProtoObject* implGetLast(ProtoContext* context);
        ProtoTupleImplementation* implGetSlice(ProtoContext* context, int from, int to);
        unsigned long implGetSize(ProtoContext* context);
        ProtoList* implAsList(ProtoContext* context);
        static ProtoTupleImplementation* tupleFromList(ProtoContext* context, ProtoList* list);
        ProtoTupleIteratorImplementation* implGetIterator(ProtoContext* context);
        ProtoTupleImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value);
        bool implHas(ProtoContext* context, ProtoObject* value);
        ProtoTupleImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value);
        ProtoTupleImplementation* implAppendFirst(ProtoContext* context, ProtoTuple* otherTuple);
        ProtoTupleImplementation* implAppendLast(ProtoContext* context, ProtoTuple* otherTuple);
        ProtoTupleImplementation* implSplitFirst(ProtoContext* context, int count);
        ProtoTupleImplementation* implSplitLast(ProtoContext* context, int count);
        ProtoTupleImplementation* implRemoveFirst(ProtoContext* context, int count);
        ProtoTupleImplementation* implRemoveLast(ProtoContext* context, int count);
        ProtoTupleImplementation* implRemoveAt(ProtoContext* context, int index);
        ProtoTupleImplementation* implRemoveSlice(ProtoContext* context, int from, int to);

        // --- Métodos de la interfaz Cell ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        unsigned long elementCount:56;
        unsigned long height:8;
        union {
            ProtoObject   *data[TUPLE_SIZE];
            ProtoTupleImplementation    *indirect[TUPLE_SIZE];
        } pointers;
    };

    // --- ProtoStringIterator ---
    // Implementación concreta para el iterador de ProtoString.
    class ProtoStringIteratorImplementation : public Cell, public ProtoStringIterator
    {
    public:
        // Constructor
        ProtoStringIteratorImplementation(
            ProtoContext* context,
            ProtoStringImplementation* base,
            unsigned long currentIndex
        );

        // Destructor
        ~ProtoStringIteratorImplementation();

        // --- Métodos de la interfaz ProtoStringIterator ---
        int implHasNext(ProtoContext* context);
        ProtoObject* implNext(ProtoContext* context);
        ProtoStringIteratorImplementation* implAdvance(ProtoContext* context);

        // --- Métodos de la interfaz Cell ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context); // Heredado de Cell, importante para la consistencia.
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        ProtoStringImplementation* base; // La string que se está iterando.
        unsigned long currentIndex; // La posición actual en la string.
    };

    // --- ProtoString ---
    // Implementación de string inmutable, basada en una tupla de caracteres.
    class ProtoStringImplementation : public Cell, public ProtoString
    {
    public:
        // Constructor
        ProtoStringImplementation(
            ProtoContext* context,
            ProtoTupleImplementation* baseTuple
        );

        // Destructor
        ~ProtoStringImplementation();

        // --- Métodos de la interfaz ProtoString ---
        int implCmpToString(ProtoContext* context, ProtoString* otherString);
        ProtoObject* implGetAt(ProtoContext* context, int index);
        unsigned long implGetSize(ProtoContext* context);
        ProtoStringImplementation* implGetSlice(ProtoContext* context, int from, int to);
        ProtoStringImplementation* implSetAt(ProtoContext* context, int index, ProtoObject* value);
        ProtoStringImplementation* implInsertAt(ProtoContext* context, int index, ProtoObject* value);
        ProtoStringImplementation* implAppendLast(ProtoContext* context, ProtoString* otherString);
        ProtoStringImplementation* implAppendFirst(ProtoContext* context, ProtoString* otherString);
        ProtoStringImplementation* implRemoveSlice(ProtoContext* context, int from, int to);
        ProtoListImplementation* implAsList(ProtoContext* context);
        ProtoStringIteratorImplementation* implGetIterator(ProtoContext* context);
        ProtoStringImplementation* implSetAtString(ProtoContext* context, int index, ProtoString* otherString);
        ProtoStringImplementation* implInsertAtString(ProtoContext* context, int index, ProtoString* otherString);
        ProtoStringImplementation* implSplitFirst(ProtoContext* context, int count);
        ProtoStringImplementation* implSplitLast(ProtoContext* context, int count);
        ProtoStringImplementation* implRemoveFirst(ProtoContext* context, int count);
        ProtoStringImplementation* implRemoveLast(ProtoContext* context, int count);
        ProtoStringImplementation* implRemoveAt(ProtoContext* context, int index);

        // --- Métodos de la interfaz Cell ---
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );

    private:
        ProtoTupleImplementation* baseTuple; // La tupla subyacente que almacena los caracteres.
    };


    // --- ProtoByteBufferImplementation ---
    // Implementación de un búfer de bytes que puede gestionar su propia memoria
    // o envolver un búfer existente.
    class ProtoByteBufferImplementation : public Cell, public ProtoByteBuffer
    {
    public:
        // Constructor: crea o envuelve un búfer de memoria.
        // Si 'buffer' es nulo, se asignará nueva memoria.
        ProtoByteBufferImplementation(
            ProtoContext* context,
            unsigned long size,
            char* buffer = nullptr
        );

        // Destructor: libera la memoria si la clase es propietaria.
        ~ProtoByteBufferImplementation();

        // --- Métodos de la interfaz ProtoByteBuffer ---
        char implGetAt(ProtoContext* context, int index);
        void implSetAt(ProtoContext* context, int index, char value);
        unsigned long implGetSize(ProtoContext* context);
        char* implGetBuffer(ProtoContext* context);

        // --- Métodos de la interfaz Cell ---
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext*, void*, Cell*)
        );
        void finalize(ProtoContext* context);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

    private:
        // Función auxiliar para validar y normalizar índices.
        bool normalizeIndex(int& index);

        unsigned long size; // El tamaño del búfer en bytes.
        char* buffer; // Puntero a la memoria del búfer.
        bool freeOnExit; // Flag que indica si el destructor debe liberar `buffer`.
    };

    // --- ProtoMethodCellImplementation ---
    // Implementación de un puntero a un método c
    class ProtoMethodCellImplementation : public Cell, public ProtoMethodCell
    {
    public:
        ProtoMethodCellImplementation(ProtoContext* context, ProtoMethod method);

        ProtoObject* implInvoke(ProtoContext* context, ProtoList* args, ProtoSparseList* kwargs);
        ProtoObject* implAsObject(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);
        void finalize(ProtoContext* context);
        void processReferences(ProtoContext* context, void* self,
                               void (*method)(ProtoContext* context, void* self, Cell* cell));
        ProtoObject* implGetSelf(ProtoContext* context);
        ProtoMethod implGetMethod(ProtoContext* context);

    private:
        ProtoMethod method{};
    };

    /**
 * @class ProtoExternalPointerImplementation
 * @brief Implementación de una celda que contiene un puntero opaco a datos externos.
 *
 * Esta clase encapsula un puntero `void*` que no es gestionado por el recolector
 * de basura de Proto. Es útil para integrar Proto con bibliotecas o datos C/C++
 * externos, permitiendo que estos punteros se pasen como objetos de primera clase.
 */
    class ProtoExternalPointerImplementation : public Cell, public ProtoExternalPointer
    {
    public:
        /**
         * @brief Constructor.
         * @param context El contexto de ejecución actual.
         * @param pointer El puntero externo (void*) que se va a encapsular.
         */
        ProtoExternalPointerImplementation(ProtoContext* context, void* pointer);

        /**
         * @brief Destructor.
         */
        ~ProtoExternalPointerImplementation();

        /**
         * @brief Obtiene el puntero externo encapsulado.
         * @param context El contexto de ejecución actual.
         * @return El puntero (void*) almacenado.
         */
        void* implGetPointer(ProtoContext* context);

        /**
         * @brief Devuelve la representación de esta celda como un ProtoObject.
         * @param context El contexto de ejecución actual.
         * @return Un ProtoObject que representa este puntero externo.
         */
        ProtoObject* implAsObject(ProtoContext* context);

        /**
         * @brief Finalizador para el recolector de basura.
         *
         * No realiza ninguna acción, ya que el puntero externo no es gestionado por el GC.
         * @param context El contexto de ejecución actual.
         */
        void finalize(ProtoContext* context);
        unsigned long getHash(ProtoContext* context);

        /**
         * @brief Procesa las referencias para el recolector de basura.
         *
         * El cuerpo está vacío porque el puntero externo no es una referencia
         * que el recolector de basura deba seguir.
         */
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell)
        );

    private:
        void* pointer; // El puntero opaco a los datos externos.
    };

    // --- ProtoThreadImplementation ---
    // La implementación interna de un hilo gestionado por el runtime de Proto.
    // Hereda de 'Cell' para ser gestionada por el recolector de basura.
    class ProtoThreadImplementation : public Cell, public ProtoThread
    {
    public:
        // --- Constructor y Destructor ---

        // Crea una nueva instancia de hilo.
        ProtoThreadImplementation(
            ProtoContext* context,
            ProtoString* name,
            ProtoSpace* space,
            ProtoMethod targetCode,
            ProtoList* args,
            ProtoSparseList* kwargs);

        // Destructor virtual para asegurar la limpieza correcta.
        virtual ~ProtoThreadImplementation();

        unsigned long getHash(ProtoContext* context);

        // --- Control de Gestión del GC ---

        // Marca el hilo como "no gestionado" para que el GC no lo detenga.
        void implSetUnmanaged();

        // Devuelve el hilo al estado "gestionado" por el GC.
        void implSetManaged();

        // --- Control del Ciclo de Vida del Hilo ---

        // Desvincula el hilo del objeto, permitiendo que se ejecute de forma independiente.
        void implDetach(ProtoContext* context);

        // Bloquea el hilo actual hasta que este hilo termine su ejecución.
        void implJoin(ProtoContext* context);

        // Solicita la finalización del hilo.
        void implExit(ProtoContext* context);

        // --- Asignación de Memoria y Sincronización ---

        // Asigna una nueva celda de memoria para el hilo.
        Cell* implAllocCell();

        // Sincroniza el hilo con el recolector de basura.
        void implSynchToGC();

        // --- Interfaz con el Sistema de Tipos ---

        // Establece el contexto de ejecución actual para el hilo.
        void implSetCurrentContext(ProtoContext* context);
        ProtoContext* implGetCurrentContext();

        // Convierte la implementación a un ProtoObject* genérico.
        ProtoObject* implAsObject(ProtoContext* context);

        // --- Métodos para el Recolector de Basura (Heredados de Cell) ---

        // Finalizador llamado por el GC antes de liberar la memoria.
        void finalize(ProtoContext* context);

        // Procesa las referencias a otras celdas para el barrido del GC.
        void processReferences(
            ProtoContext* context,
            void* self,
            void (*method)(ProtoContext* context, void* self, Cell* cell));

        static ProtoThread* implGetCurrentThread(ProtoContext* context);

        // --- Datos Miembro ---
        int state; // Estado actual del hilo respecto al GC.
        ProtoString* name; // Nombre del hilo (para depuración).
        ProtoSpace* space; // El espacio de memoria al que pertenece el hilo.
        std::thread* osThread; // El hilo real del sistema operativo.
        BigCell* freeCells; // Lista de celdas de memoria libres locales al hilo.
        ProtoContext* currentContext; // Pila de llamadas actual del hilo.
        unsigned int unmanagedCount; // Contador para llamadas anidadas a setUnmanaged/setManaged.
        struct {
            ProtoObject* object;
            ProtoObject* method_name;
            ProtoMethod method;
        } *method_cache;
    };


} // namespace proto

#endif /* PROTO_INTERNAL_H */