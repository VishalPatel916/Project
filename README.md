[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/0ek2UV58)

Assumptions:

1) Info doesn't require access permissions.
2) Other users can access the directories made by other users (nothing about this was mentioned in the problem statement)
3) If the file is at a state A, and goes to state B, undo from B will go back to A, and an undo from there will go back to B (last state before).
4) Syntax of requesting access
        REQACCESS -R|-W <filename>
    Syntax for giving access
        CHECKREQUESTS
5) write end . registers as end. while reading the file
6) ./name_server
   ./storage_server [port] [storage_path] [client_ip] [nm_ip]
   ./client_app [nm_ip]