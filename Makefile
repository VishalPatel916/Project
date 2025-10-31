# Compiler
CC = gcc

# Compiler flags: 
# -g for debugging
# -Wall for all warnings
# -Iprotocol tells gcc to look in the "protocol" folder for headers
CFLAGS = -g -Wall -Iprotocol

# --- Target Executables ---
# We now define the *full path* for the final executables
CLIENT_EXEC = client/client_app
NS_EXEC = name/name_server
SS_EXEC = storage/storage_server

# --- Object Files ---
# The .o files also go into their respective folders
CLIENT_OBJ = client/client.o
NS_OBJ = name/name_server.o
SS_OBJ = storage/storage_server.o

# --- Header Files ---
COMMON_HEADER = protocol/protocol.h

# --- Build Rules ---

# Default rule: build all three executables
all: $(CLIENT_EXEC) $(NS_EXEC) $(SS_EXEC)

# --- 1. Linking Rules ---
# Creates final executables from .o files
# $@ is the target (e.g., "client/client_app")
# The output file ($@) is now placed inside the folder
$(CLIENT_EXEC): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_OBJ)

$(NS_EXEC): $(NS_OBJ)
	$(CC) $(CFLAGS) -o $@ $(NS_OBJ)

$(SS_EXEC): $(SS_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SS_OBJ)

# --- 2. Compilation Rules ---
# Creates .o files from .c files
# $@ is the target (e.g., "client/client.o")
# $< is the source (e.g., "client/client.c")
# The output file ($@) is placed inside the folder
$(CLIENT_OBJ): client/client.c $(COMMON_HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

$(NS_OBJ): name/name_server.c $(COMMON_HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SS_OBJ): storage/storage_server.c $(COMMON_HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Cleanup Rule ---
clean:
	# rm -f will correctly remove the files from within their folders
	rm -f $(CLIENT_EXEC) $(NS_EXEC) $(SS_EXEC)
	rm -f $(CLIENT_OBJ) $(NS_OBJ) $(SS_OBJ)