/*
 * test_proto.cpp
 *
 *  A comprehensive test suite for the Proto runtime.
 *  This suite is designed to run within the ProtoSpace lifecycle,
 *  ensuring that all objects are created and managed correctly.
 *  It focuses on covering the public API defined in proto.h, avoiding
 *  any internal implementation details.
 */
#include <cstdio>
#include <cassert>
#include <string>
#include <chrono>
#include <thread>
#include "../headers/proto.h"

// --- Assertion Macro ---
// Provides clear pass/fail messages with file and line context on failure.
#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf(">> FAILED: %s (%s:%d)\n", message, __FILE__, __LINE__); \
            assert(condition); \
        } else { \
            printf("   OK: %s\n", message); \
        } \
    } while (false)

// --- Test Function Declarations ---
void test_primitives(proto::ProtoContext& c);
void test_list_operations(proto::ProtoContext& c);
void test_tuple_operations(proto::ProtoContext& c);
void test_string_operations(proto::ProtoContext& c);
void test_sparse_list_operations(proto::ProtoContext& c);
void test_prototypes_and_inheritance(proto::ProtoContext& c);
void test_byte_buffer_operations(proto::ProtoContext& c);
void test_external_pointer_operations(proto::ProtoContext& c);
void test_threading(proto::ProtoContext& c);
void test_gc_stress(proto::ProtoContext& c);


// --- Main Test Runner Function ---
// This is the entry point for the tests, executed by the ProtoSpace.
proto::ProtoObject* main_test_function(
    proto::ProtoContext* c,
    proto::ProtoObject* self,
    proto::ParentLink* parentLink,
    proto::ProtoList* args,
    proto::ProtoSparseList* kwargs
) {
    printf("=======================================\n");
    printf("      RUNNING PROTO TEST SUITE\n");
    printf("=======================================\n");

    test_primitives(*c);
    test_list_operations(*c);
    test_tuple_operations(*c);
    test_string_operations(*c);
    test_sparse_list_operations(*c);
    test_prototypes_and_inheritance(*c);
    test_byte_buffer_operations(*c);
    test_external_pointer_operations(*c);
    test_threading(*c);
    test_gc_stress(*c);

    printf("\n=======================================\n");
    printf("      ALL TESTS PASSED SUCCESSFULLY\n");
    printf("=======================================\n");

    // The test suite completed successfully, exit the process.
    exit(0);
}

// --- Executable Entry Point ---
// Its sole responsibility is to start the ProtoSpace with our test function.
int main(int argc, char **argv) {
    proto::ProtoSpace space(main_test_function, argc, argv);
    // Keep the main thread alive while the tests run in a separate thread.
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}


// --- Test Implementations ---

void test_primitives(proto::ProtoContext& c) {
    printf("\n--- Testing Primitive Types ---\n");

    // Integer tests
    proto::ProtoObject* i1 = c.fromInteger(123);
    proto::ProtoObject* i2 = c.fromInteger(-456);
    ASSERT(i1->isInteger(&c), "isInteger for positive integer");
    ASSERT(i1->asInteger(&c) == 123, "asInteger for positive integer");
    ASSERT(i2->isInteger(&c), "isInteger for negative integer");
    ASSERT(i2->asInteger(&c) == -456, "asInteger for negative integer");

    // Boolean tests
    proto::ProtoObject* b_true = c.fromBoolean(true);
    proto::ProtoObject* b_false = c.fromBoolean(false);
    ASSERT(b_true->isBoolean(&c), "isBoolean for true");
    ASSERT(b_true->asBoolean(&c) == true, "asBoolean for true");
    ASSERT(b_false->isBoolean(&c), "isBoolean for false");
    ASSERT(b_false->asBoolean(&c) == false, "asBoolean for false");

    // Type conversion tests
    ASSERT(i1->asBoolean(&c) == true, "Integer to boolean conversion (true)");
    ASSERT(c.fromInteger(0)->asBoolean(&c) == false, "Integer to boolean conversion (false)");

    // Byte tests
    proto::ProtoObject* byte = c.fromByte('A');
    ASSERT(byte->isByte(&c), "isByte for char");
    ASSERT(byte->asByte(&c) == 'A', "asByte for char");

    // Float tests
    proto::ProtoObject* f1 = c.fromDouble(3.14);
    ASSERT(f1->isFloat(&c), "isFloat for a double value");
    // Note: Floating point comparisons require a tolerance.
    ASSERT(f1->asFloat(&c) > 3.13 && f1->asFloat(&c) < 3.15, "asFloat for a double value");

    // Date tests
    proto::ProtoObject* date = c.fromDate(2024, 5, 26);
    unsigned int year, month, day;
    date->asDate(&c, year, month, day);
    ASSERT(date->isDate(&c), "isDate for a date value");
    ASSERT(year == 2024 && month == 5 && day == 26, "asDate retrieves correct date components");
}

void test_list_operations(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoList Operations ---\n");

    // Test creation and initial state
    proto::ProtoList* list = c.newList();
    ASSERT(list->getSize(&c) == 0, "A new list should be empty");
    ASSERT(list->getFirst(&c) == PROTO_NONE, "getFirst on empty list is NONE");
    ASSERT(list->getLast(&c) == PROTO_NONE, "getLast on empty list is NONE");

    // Test append and access
    proto::ProtoList* list1 = list->appendLast(&c, c.fromInteger(10));
    proto::ProtoList* list2 = list1->appendLast(&c, c.fromInteger(20));
    proto::ProtoList* list3 = list2->appendLast(&c, c.fromInteger(30));

    ASSERT(list3->getSize(&c) == 3, "List size after 3 appends");
    ASSERT(list3->getAt(&c, 0)->asInteger(&c) == 10, "getAt(0)");
    ASSERT(list3->getAt(&c, 1)->asInteger(&c) == 20, "getAt(1)");
    ASSERT(list3->getAt(&c, 2)->asInteger(&c) == 30, "getAt(2)");
    ASSERT(list3->getAt(&c, -1)->asInteger(&c) == 30, "getAt(-1) for last element");
    ASSERT(list3->getAt(&c, 99) == PROTO_NONE, "getAt() out of bounds returns NONE");

    // Test immutability
    ASSERT(list->getSize(&c) == 0, "Original list remains empty after appends");
    ASSERT(list1->getSize(&c) == 1, "Intermediate list (list1) remains unchanged");

    // Test has()
    ASSERT(list3->has(&c, c.fromInteger(20)), "has() for an existing element");
    ASSERT(!list3->has(&c, c.fromInteger(99)), "has() for a non-existing element");

    // Test setAt()
    proto::ProtoList* list4 = list3->setAt(&c, 1, c.fromInteger(25));
    ASSERT(list3->getAt(&c, 1)->asInteger(&c) == 20, "Original list is immutable after setAt");
    ASSERT(list4->getAt(&c, 1)->asInteger(&c) == 25, "New list has the updated value after setAt");
    ASSERT(list4->getSize(&c) == 3, "Size is preserved after setAt");

    // Test insertAt()
    proto::ProtoList* list5 = list4->insertAt(&c, 0, c.fromInteger(5));
    ASSERT(list5->getSize(&c) == 4, "Size after insertAt at the beginning");
    ASSERT(list5->getAt(&c, 0)->asInteger(&c) == 5, "Value after insertAt at the beginning");
    ASSERT(list5->getAt(&c, 1)->asInteger(&c) == 10, "Shifted value after insertAt");

    // Test removeAt()
    proto::ProtoList* list6 = list5->removeAt(&c, 3);
    ASSERT(list6->getSize(&c) == 3, "Size after removeAt");
    ASSERT(list6->getAt(&c, 1)->asInteger(&c) == 10, "Element before removed one");
    ASSERT(list6->getAt(&c, 2)->asInteger(&c) == 25, "Element after removed one");

    // Test getSlice()
    proto::ProtoList* slice = list3->getSlice(&c, 1, 3);
    ASSERT(slice->getSize(&c) == 2, "Slice size");
    ASSERT(slice->getAt(&c, 0)->asInteger(&c) == 20, "Slice element 0");
    ASSERT(slice->getAt(&c, 1)->asInteger(&c) == 30, "Slice element 1");

    // Test extend()
    proto::ProtoList* list_to_extend = c.newList()->appendLast(&c, c.fromInteger(40));
    proto::ProtoList* extended_list = list3->extend(&c, list_to_extend);
    ASSERT(extended_list->getSize(&c) == 4, "Size after extend");
    ASSERT(extended_list->getLast(&c)->asInteger(&c) == 40, "Last element of extended list");

    // Test iterator
    proto::ProtoListIterator* iter = list3->getIterator(&c);
    ASSERT(iter->hasNext(&c), "Iterator hasNext on a non-empty list");
    ASSERT(iter->next(&c)->asInteger(&c) == 10, "Iterator next() returns first element");
    iter = iter->advance(&c);
    ASSERT(iter->next(&c)->asInteger(&c) == 20, "Iterator next() returns second element");
    iter = iter->advance(&c);
    ASSERT(iter->next(&c)->asInteger(&c) == 30, "Iterator next() returns third element");
    iter = iter->advance(&c);
    ASSERT(!iter->hasNext(&c), "Iterator hasNext at the end is false");
}

void test_tuple_operations(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoTuple Operations ---\n");

    // Test creation from a list
    proto::ProtoList* list1 = c.newList()->appendLast(&c, c.fromInteger(1))->appendLast(&c, c.fromInteger(2));
    proto::ProtoTuple* tuple1 = c.newTupleFromList(list1);

    ASSERT(tuple1->getSize(&c) == 2, "Tuple size");
    ASSERT(tuple1->getAt(&c, 1)->asInteger(&c) == 2, "getAt on tuple");

    // Test interning: creating a tuple with the same content should yield the same object
    proto::ProtoList* list2 = c.newList()->appendLast(&c, c.fromInteger(1))->appendLast(&c, c.fromInteger(2));
    proto::ProtoTuple* tuple2 = c.newTupleFromList(list2);
    ASSERT(tuple1 == tuple2, "Interning: identical tuples should be the same object");

    // Test that different tuples are different objects
    proto::ProtoList* list3 = c.newList()->appendLast(&c, c.fromInteger(1))->appendLast(&c, c.fromInteger(3));
    proto::ProtoTuple* tuple3 = c.newTupleFromList(list3);
    ASSERT(tuple1 != tuple3, "Interning: different tuples should be different objects");

    // Test converting back to a list
    proto::ProtoList* list_from_tuple = tuple1->asList(&c);
    ASSERT(list_from_tuple->getSize(&c) == 2, "List from tuple has correct size");
    ASSERT(list_from_tuple->getAt(&c, 0)->asInteger(&c) == 1, "List from tuple has correct content");
}

void test_string_operations(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoString Operations ---\n");

    proto::ProtoString* s1 = c.fromUTF8String("hola");
    proto::ProtoString* s2 = c.fromUTF8String(" mundo");

    ASSERT(s1->getSize(&c) == 4, "String size");
    ASSERT(s1->getAt(&c, 1)->asByte(&c) == 'o', "getAt on string");

    // Test concatenation (appendLast)
    proto::ProtoString* s3 = s1->appendLast(&c, s2);
    ASSERT(s3->getSize(&c) == 10, "Concatenated string size");
    ASSERT(s1->getSize(&c) == 4, "Original string is immutable after appendLast");

    // Verify content of concatenated string
    proto::ProtoList* s3_list = s3->asList(&c);
    ASSERT(s3_list->getAt(&c, 4)->asByte(&c) == ' ', "Verify content of concatenated string");
    ASSERT(s3_list->getAt(&c, 9)->asByte(&c) == 'o', "Verify content at the end");

    // Test slicing
    proto::ProtoString* slice = s3->getSlice(&c, 5, 10);
    ASSERT(slice->getSize(&c) == 5, "String slice size");
    ASSERT(slice->getAt(&c, 0)->asByte(&c) == 'm', "String slice content");
}

void test_sparse_list_operations(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoSparseList (Dictionary) Operations ---\n");

    proto::ProtoSparseList* dict = c.newSparseList();
    ASSERT(dict->getSize(&c) == 0, "A new sparse list is empty");

    proto::ProtoString* key1 = c.fromUTF8String("name");
    proto::ProtoString* key2 = c.fromUTF8String("age");
    unsigned long key1_hash = key1->getHash(&c);
    unsigned long key2_hash = key2->getHash(&c);

    // Test setAt
    proto::ProtoSparseList* dict1 = dict->setAt(&c, key1_hash, c.fromUTF8String("proto")->asObject(&c));
    proto::ProtoSparseList* dict2 = dict1->setAt(&c, key2_hash, c.fromInteger(7));

    ASSERT(dict2->getSize(&c) == 2, "Dictionary size after two insertions");
    ASSERT(dict2->has(&c, key1_hash), "has() for an existing key");
    ASSERT(!dict2->has(&c, c.fromUTF8String("x")->getHash(&c)), "has() for a non-existing key");
    ASSERT(dict->getSize(&c) == 0, "Original dictionary is immutable after setAt");

    // Test getAt
    proto::ProtoObject* name_val = dict2->getAt(&c, key1_hash);
    ASSERT(dict2->getAt(&c, key2_hash)->asInteger(&c) == 7, "getAt() for an integer value");
    ASSERT(dict2->getAt(&c, 9999) == PROTO_NONE, "getAt() for a non-existent key returns NONE");

    // Test removeAt
    proto::ProtoSparseList* dict3 = dict2->removeAt(&c, key1_hash);
    ASSERT(dict2->has(&c, key1_hash), "Original dictionary is immutable after removeAt");
    ASSERT(dict3->getSize(&c) == 1, "New dictionary has correct size after removeAt");
    ASSERT(!dict3->has(&c, key1_hash), "Key is removed in the new dictionary");
    ASSERT(dict3->has(&c, key2_hash), "Other key is preserved in the new dictionary");
}

void test_prototypes_and_inheritance(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoObjectCell (Prototypes and Inheritance) ---\n");

    // 1. Base object and attributes
    proto::ProtoObject* base_proto = c.newObject();
    proto::ProtoString* version_attr = c.fromUTF8String("version");
    proto::ProtoString* name_attr = c.fromUTF8String("name");

    proto::ProtoObject* proto1 = base_proto->setAttribute(&c, version_attr, c.fromInteger(1));
    ASSERT(proto1->hasOwnAttribute(&c, version_attr)->asBoolean(&c), "Prototype has its own 'version' attribute");
    ASSERT(proto1->getAttribute(&c, version_attr)->asInteger(&c) == 1, "The 'version' attribute value is correct");
    ASSERT(!base_proto->hasAttribute(&c, version_attr)->asBoolean(&c), "Original base object is immutable");

    // 2. Single inheritance and attribute access
    proto::ProtoObject* child1 = proto1->newChild(&c);
    ASSERT(child1->getParents(&c)->getSize(&c) == 1, "Child has one parent");
    ASSERT(!child1->hasOwnAttribute(&c, version_attr)->asBoolean(&c), "Child does NOT have its own 'version' attribute");
    ASSERT(child1->hasAttribute(&c, version_attr)->asBoolean(&c), "Child DOES have access to inherited 'version' attribute");
    ASSERT(child1->getAttribute(&c, version_attr)->asInteger(&c) == 1, "Child inherits the correct 'version' value");
    ASSERT(child1->getAttribute(&c, name_attr) == PROTO_NONE, "Child returns NONE for a non-existent attribute");

    // 3. Attribute Shadowing
    proto::ProtoObject* child2 = child1->setAttribute(&c, version_attr, c.fromInteger(2));
    ASSERT(child1->getAttribute(&c, version_attr)->asInteger(&c) == 1, "Original child object 'child1' is not modified");
    ASSERT(child2->hasOwnAttribute(&c, version_attr)->asBoolean(&c), "New child 'child2' now has its own 'version' attribute");
    ASSERT(child2->getAttribute(&c, version_attr)->asInteger(&c) == 2, "New child 'child2' returns its own value for 'version'");

    // 4. Multiple Inheritance
    proto::ProtoObject* proto2 = base_proto->setAttribute(&c, name_attr, c.fromUTF8String("gamarino")->asObject(&c));
    proto::ProtoObject* child3 = child2->addParent(&c, proto2);

    ASSERT(child3->getParents(&c)->getSize(&c) == 2, "Child 'child3' now has two parents");
    ASSERT(child3->getAttribute(&c, version_attr)->asInteger(&c) == 2, "Child 'child3' still accesses its own 'version' attribute");
    ASSERT(child3->hasAttribute(&c, name_attr)->asBoolean(&c), "Child 'child3' can access attribute from the second parent");
    ASSERT(child3->getAttribute(&c, name_attr)->isCell(&c), "The inherited 'name' attribute is a string cell");
}

void test_byte_buffer_operations(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoByteBuffer Operations ---\n");

    // Create a new buffer
    proto::ProtoByteBuffer* buffer = c.newBuffer(10);
    ASSERT(buffer->getSize(&c) == 10, "New buffer has the correct size");

    // Set and get values
    buffer->setAt(&c, 0, 'H');
    buffer->setAt(&c, 1, 'i');
    ASSERT(buffer->getAt(&c, 0) == 'H', "getAt retrieves the correct byte");
    ASSERT(buffer->getAt(&c, 1) == 'i', "getAt retrieves the correct byte");

    // Access the raw buffer
    char* raw_buffer = buffer->getBuffer(&c);
    ASSERT(raw_buffer[0] == 'H', "Raw buffer access is correct");
}

void test_external_pointer_operations(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoExternalPointer Operations ---\n");

    // Create an external pointer to a simple integer
    int my_data = 42;
    proto::ProtoExternalPointer* ptr = c.fromExternalPointer(&my_data);

    // Retrieve the pointer and check its value
    void* retrieved_ptr = ptr->getPointer(&c);
    ASSERT(retrieved_ptr == &my_data, "Retrieved pointer matches the original");
    ASSERT(*(static_cast<int*>(retrieved_ptr)) == 42, "Value from retrieved pointer is correct");
}

// Dummy function for thread testing
proto::ProtoObject* thread_target_function(
    proto::ProtoContext* c,
    proto::ProtoObject* self,
    proto::ParentLink* parentLink,
    proto::ProtoList* args,
    proto::ProtoSparseList* kwargs
) {
    // This function simply returns the first argument it receives.
    return args->getAt(c, 0);
}

void test_threading(proto::ProtoContext& c) {
    printf("\n--- Testing ProtoThread Operations ---\n");

    // Create a new thread
    proto::ProtoList* args = c.newList()->appendLast(&c, c.fromInteger(99));
    proto::ProtoThread* thread = c.space->newThread(c.fromUTF8String("TestThread"), thread_target_function, args, nullptr);

    ASSERT(thread != nullptr, "New thread was created successfully");

    // Wait for the thread to complete
    thread->join(&c);

    // NOTE: In a more complex scenario, we would need a mechanism
    // to retrieve the return value from the thread. For this test,
    // we primarily verify that thread creation and joining do not crash.
    printf("   Thread execution and join completed without errors.\n");
}

void test_gc_stress(proto::ProtoContext& c) {
    printf("\n--- Testing Garbage Collector (GC Stress Test) ---\n");

    const int iterations = 2000;
    const int keep_every = 100;

    // Create a root list that will keep some objects alive.
    proto::ProtoList* root_list = c.newList();

    // Create many temporary objects. Most will be garbage.
    for (int i = 0; i < iterations; ++i) {
        proto::ProtoList* tempList = c.newList();
        tempList = tempList->appendLast(&c, c.fromInteger(i));
        tempList = tempList->appendLast(&c, c.fromUTF8String("temporary string data")->asObject(&c));

        // Every 'keep_every' iterations, add the list to our root list.
        if (i % keep_every == 0) {
            root_list = root_list->appendLast(&c, tempList->asObject(&c));
        }
    }

    ASSERT(root_list->getSize(&c) == iterations / keep_every, "Root list has the correct number of persistent objects");

    printf("   Forcing Garbage Collection...\n");
    c.space->triggerGC();
    // Give the GC thread time to run.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify that the objects kept in the root list are still valid.
    proto::ProtoList* first_kept = root_list->getAt(&c, 0)->asList(&c);
    ASSERT(first_kept->getAt(&c, 0)->asInteger(&c) == 0, "First persistent object is still valid after GC");

    proto::ProtoList* last_kept = root_list->getAt(&c, (iterations / keep_every) - 1)->asList(&c);
    ASSERT(last_kept->getAt(&c, 0)->asInteger(&c) == iterations - keep_every, "Last persistent object is still valid after GC");

    printf("   GC Stress Test completed without failures.\n");
}
