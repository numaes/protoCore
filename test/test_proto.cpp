/*
 * test_proto.cpp
 *
 *  Created on: 2024-01-15
 *      Author: gamarino
 *
 *  This file contains the main test suite for the Proto library,
 *  covering primitives, lists, objects, inheritance, and more.
 */

#include <iostream>
#include <vector>
#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

// Forward declarations of test functions
void test_primitives(ProtoContext& c);
void test_list_operations(ProtoContext& c);
void test_tuple_operations(ProtoContext& c);
void test_string_operations(ProtoContext& c);
void test_sparse_list_operations(ProtoContext& c);
void test_prototypes_and_inheritance(ProtoContext& c);
void test_gc_stress(ProtoContext& c);

// Main test function to be called by the ProtoSpace
const ProtoObject* main_test_function(
    ProtoContext* c,
    const ProtoObject* self,
    const ParentLink* parentLink,
    const ProtoList* positionalParameters,
    const ProtoSparseList* keywordParametersDict
) {
    std::cout << "Running all tests..." << std::endl;
    test_primitives(*c);
    test_list_operations(*c);
    test_tuple_operations(*c);
    test_string_operations(*c);
    test_sparse_list_operations(*c);
    test_prototypes_and_inheritance(*c);
    test_gc_stress(*c);
    std::cout << "All tests passed!" << std::endl;
    return PROTO_NONE;
}


// --- Test Implementations ---

void test_primitives(ProtoContext& c) {
    // Corrected: Use const pointers
    const proto::ProtoObject* i1 = c.fromInteger(123);
    const proto::ProtoObject* i2 = c.fromInteger(-456);
    const proto::ProtoObject* b_true = c.fromBoolean(true);
    const proto::ProtoObject* b_false = c.fromBoolean(false);
    const proto::ProtoObject* byte = c.fromByte('A');

    ASSERT_TRUE(i1->isInteger(&c));
    ASSERT_EQ(i1->asLong(&c), 123);
    ASSERT_TRUE(b_true->isBoolean(&c));
    ASSERT_TRUE(b_true->asBoolean(&c));
    ASSERT_EQ(byte->asByte(&c), 'A');
}

void test_list_operations(ProtoContext& c) {
    // Corrected: Use const pointers
    const proto::ProtoList* list = c.newList();
    ASSERT_EQ(list->getSize(&c), 0);

    const proto::ProtoList* list1 = list->appendLast(&c, c.fromInteger(10));
    const proto::ProtoList* list2 = list1->appendLast(&c, c.fromInteger(20));
    const proto::ProtoList* list3 = list2->appendLast(&c, c.fromInteger(30));

    ASSERT_EQ(list3->getSize(&c), 3);
    ASSERT_EQ(list3->getAt(&c, 1)->asLong(&c), 20);

    const proto::ProtoList* list4 = list3->setAt(&c, 1, c.fromInteger(25));
    ASSERT_EQ(list4->getAt(&c, 1)->asLong(&c), 25);
    ASSERT_EQ(list3->getAt(&c, 1)->asLong(&c), 20); // Immutability

    const proto::ProtoList* list5 = list4->insertAt(&c, 0, c.fromInteger(5));
    ASSERT_EQ(list5->getAt(&c, 0)->asLong(&c), 5);
    ASSERT_EQ(list5->getAt(&c, 1)->asLong(&c), 10);

    const proto::ProtoList* list6 = list5->removeAt(&c, 3);
    ASSERT_EQ(list6->getSize(&c), 4);

    const proto::ProtoList* slice = list3->getSlice(&c, 1, 3);
    ASSERT_EQ(slice->getSize(&c), 2);
    ASSERT_EQ(slice->getAt(&c, 0)->asLong(&c), 20);

    const proto::ProtoList* emptySlice = list3->getSlice(&c, 1, 1);
    ASSERT_EQ(emptySlice->getSize(&c), 0);

    const proto::ProtoListIterator* iter = list3->getIterator(&c);
    ASSERT_TRUE(iter->hasNext(&c));
    ASSERT_EQ(iter->next(&c)->asLong(&c), 10);
    iter = iter->advance(&c);
    ASSERT_EQ(iter->next(&c)->asLong(&c), 20);
    iter = iter->advance(&c);
    ASSERT_EQ(iter->next(&c)->asLong(&c), 30);
    iter = iter->advance(&c);
    ASSERT_FALSE(iter->hasNext(&c));
}

void test_tuple_operations(ProtoContext& c) {
    // Corrected: Use const pointers
    const proto::ProtoList* list1 = c.newList()->appendLast(&c, c.fromInteger(1))->appendLast(&c, c.fromInteger(2));
    const proto::ProtoTuple* tuple1 = c.newTupleFromList(list1);
    ASSERT_EQ(tuple1->getSize(&c), 2);
    ASSERT_EQ(tuple1->getAt(&c, 1)->asLong(&c), 2);

    const proto::ProtoList* list2 = c.newList()->appendLast(&c, c.fromInteger(1))->appendLast(&c, c.fromInteger(2));
    const proto::ProtoTuple* tuple2 = c.newTupleFromList(list2);
    ASSERT_EQ(tuple1, tuple2); // Interning check

    const proto::ProtoList* list3 = c.newList()->appendLast(&c, c.fromInteger(1))->appendLast(&c, c.fromInteger(3));
    const proto::ProtoTuple* tuple3 = c.newTupleFromList(list3);
    ASSERT_NE(tuple1, tuple3);
}

void test_string_operations(ProtoContext& c) {
    // Corrected: Use asString for conversion
    const proto::ProtoString* s1 = c.fromUTF8String("hola")->asString(&c);
    const proto::ProtoString* s2 = c.fromUTF8String(" mundo")->asString(&c);
    ASSERT_EQ(s1->getSize(&c), 4);
    ASSERT_EQ(s2->getSize(&c), 6);

    const proto::ProtoString* s3 = s1->appendLast(&c, s2);
    ASSERT_EQ(s3->getSize(&c), 10);

    const proto::ProtoList* s3_list = s3->asList(&c);
    ASSERT_EQ(s3_list->getSize(&c), 10);

    const proto::ProtoString* slice = s3->getSlice(&c, 5, 10);
    ASSERT_EQ(slice->getSize(&c), 5);
}

void test_sparse_list_operations(ProtoContext& c) {
    // Corrected: Use const pointers
    const proto::ProtoSparseList* dict = c.newSparseList();
    ASSERT_EQ(dict->getSize(&c), 0);

    const proto::ProtoString* key1 = c.fromUTF8String("name")->asString(&c);
    const proto::ProtoString* key2 = c.fromUTF8String("age")->asString(&c);
    const unsigned long key1_hash = key1->getHash(&c);
    const unsigned long key2_hash = key2->getHash(&c);

    const proto::ProtoSparseList* dict1 = dict->setAt(&c, key1_hash, c.fromUTF8String("protoCore"));
    const proto::ProtoSparseList* dict2 = dict1->setAt(&c, key2_hash, c.fromInteger(7));

    ASSERT_EQ(dict2->getSize(&c), 2);
    ASSERT_TRUE(dict2->has(&c, key1_hash));
    
    const proto::ProtoObject* name_val = dict2->getAt(&c, key1_hash);
    ASSERT_TRUE(name_val->isString(&c));

    const proto::ProtoSparseList* dict3 = dict2->removeAt(&c, key1_hash);
    ASSERT_EQ(dict3->getSize(&c), 1);
    ASSERT_FALSE(dict3->has(&c, key1_hash));
}

void test_prototypes_and_inheritance(ProtoContext& c) {
    // Corrected: Use const pointers, use const_cast for mutable operations
    const proto::ProtoObject* base_proto = c.newObject();
    const proto::ProtoString* version_attr = c.fromUTF8String("version")->asString(&c);
    const proto::ProtoString* name_attr = c.fromUTF8String("name")->asString(&c);

    const proto::ProtoObject* proto1 = base_proto->setAttribute(&c, const_cast<ProtoString*>(version_attr), c.fromInteger(1));
    ASSERT_EQ(proto1->getAttribute(&c, const_cast<ProtoString*>(version_attr))->asLong(&c), 1);

    const proto::ProtoObject* child1 = proto1->newChild(&c);
    ASSERT_TRUE(child1->isInstanceOf(&c, proto1));
    ASSERT_EQ(child1->getAttribute(&c, const_cast<ProtoString*>(version_attr))->asLong(&c), 1);

    const proto::ProtoObject* child2 = child1->setAttribute(&c, const_cast<ProtoString*>(version_attr), c.fromInteger(2));
    ASSERT_EQ(child2->getAttribute(&c, const_cast<ProtoString*>(version_attr))->asLong(&c), 2);
    ASSERT_EQ(child1->getAttribute(&c, const_cast<ProtoString*>(version_attr))->asLong(&c), 1);

    const proto::ProtoObject* proto2 = base_proto->setAttribute(&c, const_cast<ProtoString*>(name_attr), c.fromUTF8String("gamarino"));
    const proto::ProtoObject* child3 = const_cast<proto::ProtoObject*>(child2)->addParent(&c, proto2);
    ASSERT_TRUE(child3->getAttribute(&c, const_cast<ProtoString*>(name_attr))->isString(&c));
}

void test_gc_stress(ProtoContext& c) {
    const int iterations = 5000;
    const int keep_every = 100;
    
    const proto::ProtoList* root_list = c.newList();

    for (int i = 0; i < iterations; ++i) {
        const proto::ProtoList* tempList = c.newList();
        tempList = tempList->appendLast(&c, c.fromInteger(i));
        tempList = tempList->appendLast(&c, c.fromUTF8String("temporary string data"));
        
        if (i % keep_every == 0) {
            root_list = root_list->appendLast(&c, tempList->asObject(&c));
        }
    }
    
    c.space->triggerGC();
    
    ASSERT_EQ(root_list->getSize(&c), iterations / keep_every);
    
    const proto::ProtoList* first_kept = root_list->getAt(&c, 0)->asList(&c);
    ASSERT_EQ(first_kept->getSize(&c), 2);
    
    const proto::ProtoList* last_kept = root_list->getAt(&c, (iterations / keep_every) - 1)->asList(&c);
    ASSERT_EQ(last_kept->getAt(&c, 0)->asLong(&c), iterations - keep_every);
}
