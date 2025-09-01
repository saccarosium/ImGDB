// GDB Layer

#pragma once

// Frozen Var Objects
#define GDB_FRVAROBJ BitField(0)
// Pending Breakpoints
#define GDB_PBREAK BitField(1)
// Python Scripting
#define GDB_PYTHON BitField(2)
// Thread Info
#define GDB_THINFO BitField(3)
// Commands -data-read-memory bytes and -data-write-memory-bytes
#define GDB_RWBYTES BitField(4)
// Async Breakpoints
#define GDB_ASBREAK BitField(5) // Language
// Ada Task Info
#define GDB_ADA BitField(6)
// Language Option
#define GDB_LANGOPT BitField(7)
// Mi Command
#define GDB_MICMD BitField(8)
// Data Disasseble
#define GDB_DATADIS BitField(9)
// Async Execution
#define GDB_ASEXE BitField(10)
// Reverse Execution
#define GDB_REVEXE BitField(11)
// Run Start
#define GDB_RUNSTART BitField(12)


struct GDB {
    pid_t spawned_pid; // process running GDB
    String debug_filename; // debug executable filename
    String debug_args; // args passed to debug executable
    String ptty_slave;
    i32 fd_ptty_master;

    pthread_t thread_read_interp;
    // pthread_t thread_write_stdin;

    sem_t* recv_block;
    pthread_mutex_t modify_block;

    // MI command sent from GDB
    i32 fd_in_read;
    i32 fd_in_write;

    // commands sent to GDB
    i32 fd_out_read;
    i32 fd_out_write;

    // ordinal ID that gets incremented on every
    // GDB_SendBlocking record sent
    u32 record_id = 1;

    // raw data, guarded by modify_storage_lock
    // a block is one or more Records
    char block_data[MB(1)];
    std::vector<Span> block_spans; // pipe read span into block_data

    u32 capabilities = 0;
};

extern GDB g_gdb;

struct ParseRecordContext {
    std::vector<RecordAtom> atoms;
    size_t atom_idx;
    size_t num_end_atoms; // contiguous atoms stored at the end of atoms

    bool error;

    size_t i;
    const char* buf; // record line data
    size_t bufsize;
};

// traverse through all the child elements of an array/struct
struct AtomIter {
    const RecordAtom* iter_begin;
    const RecordAtom* iter_end;
    const RecordAtom* begin() { return iter_begin; }
    const RecordAtom* end() { return iter_end; }
};

namespace gdb {

AtomIter iter_child(const Record& rec, const RecordAtom* array);

// extract values from parsed records
String extract_value(const char* name, const RecordAtom& root, const Record& rec);
int extract_int(const char* name, const RecordAtom& root, const Record& rec);
const RecordAtom* extract_atom(const char* name, const RecordAtom& root, const Record& rec);

// helper functions for searching the root node of a record
String extract_value(const char* name, const Record& rec);
int extract_int(const char* name, const Record& rec);
const RecordAtom* extract_atom(const char* name, const Record& rec);

inline String get_atom_string(Span s, const Record& rec)
{
    Assert(s.index + s.length <= rec.buf.size());
    String result = {};
    result.assign(rec.buf.data() + s.index, s.length);
    return result;
}

bool start_process(String gdb_filename, String gdb_args);

bool set_inferior_exe(String filename);

bool set_inferior_args(String args);

void shutdown();

// send a message to GDB, don't wait for result
bool send(const char* cmd);

// send a message to GDB, wait for a result record
bool send_blocking(const char* cmd, bool remove_after = true);

// send a message to GDB, wait for a result record, then retrieve it
bool send_blocking(const char* cmd, Record& rec);

// extract a MI record from a newline terminated line
bool parse_record(char* buf, size_t bufsize, ParseRecordContext& ctx);

// first word after record type char
// ex: ^done, *stopped
String get_record_action(const Record& rec);

void grab_block_data();

RecordAtomSequence recurse_evaluation(ParseRecordContext& ctx);

typedef void AtomIterator(Record& rec, RecordAtom& iter, void* ctx);
void iterate_atoms(Record& rec, RecordAtom& iter, AtomIterator* iterator, void* ctx);

void print_record_atom(
    const Record& rec, const RecordAtom& iter, int tab_level, FILE* out = stdout);

} // namespace gdb
