#!/bin/bash

# Test 1: Basic word insertion (no delimiters)
echo "=== Test 1: Basic insertion ==="
{
    echo "test1"
    sleep 0.3
    echo "create t1.txt"
    sleep 0.3
    echo "write t1.txt"
    echo "Hello world."
    sleep 0.3
    echo "write t1.txt 0"
    sleep 0.3
    echo "1 beautiful"
    sleep 0.3
    echo "ETIRW"
    sleep 0.5
    echo "read t1.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"

# Test 2: Insert word with delimiter (sentence split)
echo ""
echo "=== Test 2: Insert with delimiter ==="
{
    echo "test2"
    sleep 0.3
    echo "create t2.txt"
    sleep 0.3
    echo "write t2.txt"
    echo "Hello world."
    sleep 0.3
    echo "write t2.txt 0"
    sleep 0.3
    echo "1 there."
    sleep 0.3
    echo "ETIRW"
    sleep 0.5
    echo "read t2.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"

# Test 3: Insert standalone delimiter
echo ""
echo "=== Test 3: Standalone delimiter ==="
{
    echo "test3"
    sleep 0.3
    echo "create t3.txt"
    sleep 0.3
    echo "write t3.txt"
    echo "Hello world"
    sleep 0.3
    echo "write t3.txt 0"
    sleep 0.3
    echo "2 ."
    sleep 0.3
    echo "ETIRW"
    sleep 0.5
    echo "read t3.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"

# Test 4: Multiple edits in session
echo ""
echo "=== Test 4: Multiple edits ==="
{
    echo "test4"
    sleep 0.3
    echo "create t4.txt"
    sleep 0.3
    echo "write t4.txt"
    echo "Hello world."
    sleep 0.3
    echo "write t4.txt 0"
    sleep 0.3
    echo "1 beautiful"
    sleep 0.3
    echo "3 today"
    sleep 0.3
    echo "ETIRW"
    sleep 0.5
    echo "read t4.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"

# Test 5: Delimiter preservation after split
echo ""
echo "=== Test 5: Delimiter preservation ==="
{
    echo "test5"
    sleep 0.3
    echo "create t5.txt"
    sleep 0.3
    echo "write t5.txt"
    echo "First second third."
    sleep 0.3
    echo "write t5.txt 0"
    sleep 0.3
    echo "1 mid."
    sleep 0.3
    echo "ETIRW"
    sleep 0.5
    echo "read t5.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"

# Test 6: Insert at position 0
echo ""
echo "=== Test 6: Insert at position 0 ==="
{
    echo "test6"
    sleep 0.3
    echo "create t6.txt"
    sleep 0.3
    echo "write t6.txt"
    echo "world."
    sleep 0.3
    echo "write t6.txt 0"
    sleep 0.3
    echo "0 Hello"
    sleep 0.3
    echo "ETIRW"
    sleep 0.5
    echo "read t6.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"

echo ""
echo "=== All tests complete ==="
