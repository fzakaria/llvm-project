R"(-- typedef struct Rela {
--    uint32_t id; // Section header index
--    uint64_t r_offset; // Location (file byte offset, or program virtual addr).
--    uint64_t r_info;  // Symbol table index and type of relocation to apply.
--    int64_t r_addend; // Compute value for relocatable field by adding this.
--  } Rela;
CREATE TABLE IF NOT EXISTS Relocation
(
    address
    INTEGER,
    r_info,
    INTEGER,
    r_addend
    INTEGER
);)"