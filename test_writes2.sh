#!/bin/bash

echo "=== Test 1: Basic insertion ==="
{
    echo "u1"
    sleep 0.5
    echo "createfolder test_folder"
    sleep 0.5
    echo "create test_folder/t1.txt"
    sleep 0.5
    # First write initial content using echo/redirect
} | ./client_app 2>&1 > /dev/null

# Create initial file manually
echo "Hello world." > ss_storage/test_folder/t1.txt

{
    echo "u1"
    sleep 0.5
    echo "write test_folder/t1.txt 0"
    sleep 0.5
    echo "1 beautiful"
    sleep 0.5
    echo "ETIRW"
    sleep 0.5
    echo "read test_folder/t1.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"
echo "Expected: Hello beautiful world."

echo ""
echo "=== Test 2: Insert with delimiter ==="
echo "Hello world." > ss_storage/test_folder/t2.txt
{
    echo "u1"
    sleep 0.5
    echo "write test_folder/t2.txt 0"
    sleep 0.5
    echo "1 there."
    sleep 0.5
    echo "ETIRW"
    sleep 0.5
    echo "read test_folder/t2.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"
echo "Expected: Hello there. world."

echo ""
echo "=== Test 3: Standalone delimiter ==="
echo "Hello world" > ss_storage/test_folder/t3.txt
{
    echo "u1"
    sleep 0.5
    echo "write test_folder/t3.txt 0"
    sleep 0.5
    echo "2 ."
    sleep 0.5
    echo "ETIRW"
    sleep 0.5
    echo "read test_folder/t3.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"
echo "Expected: Hello world."

echo ""
echo "=== Test 4: Multiple edits ==="
echo "Hello world." > ss_storage/test_folder/t4.txt
{
    echo "u1"
    sleep 0.5
    echo "write test_folder/t4.txt 0"
    sleep 0.5
    echo "1 beautiful"
    sleep 0.5
    echo "3 today"
    sleep 0.5
    echo "ETIRW"
    sleep 0.5
    echo "read test_folder/t4.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"
echo "Expected: Hello beautiful world today."

echo ""
echo "=== Test 5: Delimiter preservation ==="
echo "First second third." > ss_storage/test_folder/t5.txt
{
    echo "u1"
    sleep 0.5
    echo "write test_folder/t5.txt 0"
    sleep 0.5
    echo "1 mid."
    sleep 0.5
    echo "ETIRW"
    sleep 0.5
    echo "read test_folder/t5.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"
echo "Expected: First mid. second third."

echo ""
echo "=== Test 6: Insert at position 0 ==="
echo "world." > ss_storage/test_folder/t6.txt
{
    echo "u1"
    sleep 0.5
    echo "write test_folder/t6.txt 0"
    sleep 0.5
    echo "0 Hello"
    sleep 0.5
    echo "ETIRW"
    sleep 0.5
    echo "read test_folder/t6.txt"
    sleep 0.5
    echo "exit"
} | ./client_app 2>&1 | grep -A 2 "Start of file"
echo "Expected: Hello world."

echo ""
echo "=== All tests complete ==="
