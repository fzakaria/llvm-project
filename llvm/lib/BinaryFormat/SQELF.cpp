#include "llvm/BinaryFormat/SQELF.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include <sqlite3.h>

using namespace llvm;
using namespace BinaryFormat;


SQELF::SQELF() {
  int rc = sqlite3_open(":memory:", &DB);
  if (rc != SQLITE_OK) {
    report_fatal_error("Could not create an in-memory sqlite database");
  }
  initializeTables();
}

SQELF::~SQELF() {
  int rc = sqlite3_close(DB);
  if (rc != SQLITE_OK) {
    report_fatal_error(
        "Could not close in-memory sqlite database; likely database is locked");
  }
}

namespace llvm {
namespace BinaryFormat {

// TODO(fzakaria): Is there a better preferred way to create large
// text files?

void SQELF::initializeTables() {

  char *errMsg = nullptr;
  int rc =
      sqlite3_exec(DB, CREATE_METADATA_TABLE_SQL, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    report_fatal_error(
        formatv("failed to create sqlite3 meta data table: {0}", std::string(errMsg)));
    sqlite3_free(errMsg);
  }
  rc = sqlite3_exec(DB, CREATE_RELOCATION_TABLE_SQL, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    report_fatal_error(
        formatv("failed to create sqlite3 relocation table: {0}", std::string(errMsg)));
    sqlite3_free(errMsg);
  }
  rc =
      sqlite3_exec(DB, CREATE_INSTRUCTION_TABLE_SQL, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    report_fatal_error(
        formatv("failed to create sqlite3 instruction table: {0}", std::string(errMsg)));
    sqlite3_free(errMsg);
  }
}

/**
 * @brief The SQELF ObjectFormat stores it's internal representation as an
 * in-memory database. We however want to pipe this to the object stream.
 * This function handles that conversion by first dumping the database
 * to a temporary file.
 */
void SQELF::writeInMemoryDatabaseToStream(llvm::raw_ostream &OS) {
  llvm::SmallString<64> tempFilename;
  if (llvm::sys::fs::createTemporaryFile("temp", "db", tempFilename)) {
    report_fatal_error("Could not create temporary file");
    return;
  }

  // Save the in-memory database to a temporary file.
  sqlite3 *tempDb;
  if (sqlite3_open(tempFilename.c_str(), &tempDb) != SQLITE_OK) {
    report_fatal_error("failed to open sqlite3 database: " + tempFilename);
    return;
  }

  sqlite3_backup *backup = sqlite3_backup_init(tempDb, "main", DB, "main");
  if (backup) {
    sqlite3_backup_step(backup, -1);
    sqlite3_backup_finish(backup);
  }

  if (sqlite3_close(tempDb) != SQLITE_OK) {
    report_fatal_error("failed to close sqlite3 database: " + tempFilename);
    return;
  }

  // Open the temporary file
  auto fileBuffer = llvm::MemoryBuffer::getFile(tempFilename.c_str());

  // Write the temporary file contents to the output stream.
  OS << fileBuffer->get()->getBuffer();

  // Delete the temporary file.
  std::remove(tempFilename.c_str());
}

void SQELF::writeRelocationToDatabase(const SQELF::Rela &R) {
  // TODO
}
void SQELF::writeInstructionToDatabase(const SQELF::Ins &I) {
  int rc = sqlite3_open(":memory:", &DB);
  if (rc != SQLITE_OK) {
    report_fatal_error("Could not create an in-memory sqlite database");
  }

  sqlite3_stmt *stmt;
  const char *sql = "INSERT INTO Ins VALUES(?, ?, ?, ?, ?, ?)";
  rc = sqlite3_prepare_v2(DB, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    report_fatal_error(
        "Could not prepare INSERT statement in an in-memory sqlite database");
  }

  const char *mnemonic = I.mnemonic.data();
  std::string operandString;
  llvm::MCOperand operand1 = I.operand1;
  llvm::MCOperand operand2 = I.operand2;
  llvm::MCOperand operand3 = I.operand3;

  rc |= sqlite3_bind_int64(stmt, 1, I.address);
  rc |= sqlite3_bind_text(stmt, 2, mnemonic, -1, SQLITE_STATIC);

  if (operand1.isExpr()) {
    const llvm::MCExpr *expr = operand1.getExpr();
    llvm::raw_string_ostream(operandString) << *expr;
  } else if (operand1.isImm()) {
    operandString = std::to_string(operand1.getImm());
  } else if (operand1.isReg()) {
    // Handle register operands if needed
    unsigned regNum = operand1.getReg();
    operandString = "register_" + std::to_string(regNum);
  } else {
    // Handle other cases if needed
    operandString = "unknown_operand";
  }
  rc |= sqlite3_bind_text(stmt, 3, operandString.c_str(), -1, SQLITE_STATIC);

  if (operand2.isExpr()) {
    const llvm::MCExpr *expr = operand2.getExpr();
    llvm::raw_string_ostream(operandString) << *expr;
  } else if (operand2.isImm()) {
    operandString = std::to_string(operand2.getImm());
  } else if (operand2.isReg()) {
    // Handle register operands if needed
    unsigned regNum = operand2.getReg();
    operandString = "register_" + std::to_string(regNum);
  } else {
    // Handle other cases if needed
    operandString = "unknown_operand";
  }
  rc |= sqlite3_bind_text(stmt, 4, operandString.c_str(), -1, SQLITE_STATIC);
  if (operand3.isExpr()) {
    const llvm::MCExpr *expr = operand3.getExpr();
    llvm::raw_string_ostream(operandString) << *expr;
  } else if (operand3.isImm()) {
    operandString = std::to_string(operand3.getImm());
  } else if (operand3.isReg()) {
    // Handle register operands if needed
    unsigned regNum = operand3.getReg();
    operandString = "register_" + std::to_string(regNum);
  } else {
    // Handle other cases if needed
    operandString = "unknown_operand";
  }
  rc |= sqlite3_bind_text(stmt, 5, operandString.c_str(), -1, SQLITE_STATIC);

  if (rc != SQLITE_OK) {
    report_fatal_error(
        "Could not bind to the statement in an in-memory sqlite database");
  }
  sqlite3_finalize(stmt);
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SQELF &BF) {
  BF.writeInMemoryDatabaseToStream(OS);
  return OS;
}
} // namespace BinaryFormat
} // namespace llvm
