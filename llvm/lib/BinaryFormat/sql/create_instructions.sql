R"(-- typedef struct Ins {
--    uint32_t id;
--    uint32_t address;
--    std::string mnemonic;
--    std::string operand1;
--    std::string operand2;
--    std::string operand3;
--  } Ins 

CREATE TABLE IF NOT EXISTS Ins
(   
    id
    INTEGER,
    address
    INTEGER,
    mnemonic
    VARCHAR,
    operand1
    VARCHAR,
    operand2
    VARCHAR,
    operand3
    VARCHAR,
    PRIMARY
    KEY
    (id, address)
);)"