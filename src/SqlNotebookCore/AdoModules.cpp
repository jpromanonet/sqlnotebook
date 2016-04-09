// SQL Notebook
// Copyright (C) 2016 Brian Luft
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <msclr/auto_handle.h>
#include "gcroot.h"
#include "SqlNotebookCore.h"
using namespace SqlNotebookCore;
using namespace System::Data;
using namespace System::Data::SqlClient;
using namespace System::IO;
using namespace System::Text;
using namespace Npgsql;
using namespace msclr;
using namespace MySql::Data::MySqlClient;

private struct AdoCreateInfo {
    public:
    gcroot<Func<String^, IDbConnection^>^> ConnectionCreator;
};

private struct AdoTable {
    sqlite3_vtab Super;
    gcroot<String^> ConnectionString;
    gcroot<String^> AdoTableName;
    gcroot<List<String^>^> ColumnNames;
    gcroot<Func<String^, IDbConnection^>^> ConnectionCreator;
    int64_t InitialRowCount;
    AdoTable() {
        memset(&Super, 0, sizeof(Super));
    }
};

private struct AdoCursor {
    sqlite3_vtab_cursor Super;
    AdoTable* Table;
    gcroot<IDbConnection^> Connection;
    gcroot<IDbCommand^> Command;
    gcroot<IDataReader^> Reader;
    bool IsEof;

    AdoCursor() {
        memset(&Super, 0, sizeof(Super));
        Table = nullptr;
    }
};

static int AdoCreate(sqlite3* db, void* pAux, int argc, const char* const* argv, sqlite3_vtab** ppVTab, char** pzErr) {
    // argv[3]: connectionString
    // argv[4]: table name

    AdoTable* vtab = nullptr;
    auto createInfo = (AdoCreateInfo*)pAux;

    try {
        if (argc != 5) {
            throw gcnew Exception("Syntax: CREATE VIRTUAL TABLE <name> USING pgsql ('<connection string>', 'table name');");
        }
        auto connStr = Util::Str(argv[3])->Trim(L'\'');
        auto adoTableName = Util::Str(argv[4])->Trim(L'\'');
        auto_handle<IDbConnection> conn(createInfo->ConnectionCreator->Invoke(connStr));
        conn->Open();

        // ensure the table exists and detect the column names
        auto columnNames = gcnew List<String^>();
        auto columnTypes = gcnew List<Type^>();
        {
            auto_handle<IDbCommand> cmd(conn->CreateCommand());
            cmd->CommandText = "SELECT * FROM \"" + adoTableName->Replace("\"", "\"\"") + "\" WHERE 1 = 0";
            auto_handle<IDataReader> reader(cmd->ExecuteReader());
            for (int i = 0; i < reader->FieldCount; i++) {
                columnNames->Add(reader->GetName(i));
                columnTypes->Add(reader->GetFieldType(i));
            }
        }

        // get a row count too
        int64_t rowCount = 0;
        {
            auto_handle<IDbCommand> cmd(conn->CreateCommand());
            cmd->CommandText = "SELECT COUNT(*) FROM \"" + adoTableName->Replace("\"", "\"\"") + "\"";
            rowCount = Convert::ToInt64(cmd->ExecuteScalar());
        }

        // create sqlite structure
        auto vtab = new AdoTable;
        vtab->ConnectionString = connStr;
        vtab->AdoTableName = adoTableName;
        vtab->ColumnNames = columnNames;
        vtab->InitialRowCount = rowCount;
        vtab->ConnectionCreator = createInfo->ConnectionCreator;

        // register the column names and types with pgsql
        auto columnLines = gcnew List<String^>();
        for (int i = 0; i < columnNames->Count; i++) {
            auto t = columnTypes[i];
            String^ sqlType;
            if (t == Int16::typeid || t == Int32::typeid || t == Int64::typeid || t == Byte::typeid || t == Boolean::typeid) {
                sqlType = "integer";
            } else if (t == Single::typeid || t == Double::typeid || t == Decimal::typeid) {
                sqlType = "real";
            } else {
                sqlType = "text";
            }
            columnLines->Add("\"" + columnNames[i]->Replace("\"", "\"\"") + "\" " + sqlType);
        }
        auto createSql = "CREATE TABLE a (" + String::Join(", ", columnLines) + ")";
        g_SqliteCall(db, sqlite3_declare_vtab(db, Util::CStr(createSql).c_str()));

        *ppVTab = &vtab->Super;
        return SQLITE_OK;
    } catch (Exception^ ex) {
        delete vtab;
        *pzErr = sqlite3_mprintf("AdoCreate: %s", Util::CStr(ex->Message).c_str());
        return SQLITE_ERROR;
    }
}

static int AdoDestroy(sqlite3_vtab* pVTab) {
    delete (AdoTable*)pVTab;
    return SQLITE_OK;
}

static int AdoBestIndex(sqlite3_vtab* pVTab, sqlite3_index_info* info) {
    auto vtab = (AdoTable*)pVTab;
    
    // build a query corresponding to the request
    auto sb = gcnew StringBuilder();
    sb->Append("SELECT * FROM \"");
    sb->Append(vtab->AdoTableName);
    sb->Append("\"");

    // where clause
    int argvIndex = 1;
    if (info->nConstraint > 0) {
        sb->Append(" WHERE ");
        auto terms = gcnew List<String^>();
        for (int i = 0; i < info->nConstraint; i++) {
            if (info->aConstraint[i].iColumn == -1) {
                continue; // rowid instead of a column. we don't support this type of constraint.
            } else if (!info->aConstraint[i].usable) {
                continue;
            }

            String^ op;
            switch (info->aConstraint[i].op) {
                case SQLITE_INDEX_CONSTRAINT_EQ: op = " = "; break;
                case SQLITE_INDEX_CONSTRAINT_GT: op = " > "; break;
                case SQLITE_INDEX_CONSTRAINT_LE: op = " <= "; break;
                case SQLITE_INDEX_CONSTRAINT_LT: op = " < "; break;
                case SQLITE_INDEX_CONSTRAINT_GE: op = " >= "; break;
                case SQLITE_INDEX_CONSTRAINT_LIKE: op = " LIKE "; break;
                default: continue; // we don't support this operator
            }

            info->aConstraintUsage[i].argvIndex = argvIndex;
            info->aConstraintUsage[i].omit = true;
            terms->Add(vtab->ColumnNames->default[info->aConstraint[i].iColumn] + op + "@arg" + argvIndex);
            argvIndex++;
        }
        sb->Append(String::Join(" AND ", terms));
    }

    // order by clause
    if (info->nOrderBy > 0) {
        sb->Append(" ORDER BY ");
        auto terms = gcnew List<String^>();
        for (int i = 0; i < info->nOrderBy; i++) {
            terms->Add(vtab->ColumnNames->default[info->aOrderBy[i].iColumn] + (info->aOrderBy[i].desc ? " DESC" : ""));
        }
        sb->Append(String::Join(", ", terms));
        info->orderByConsumed = true;
    }

    auto sql = Util::CStr(sb->ToString());
    info->idxNum = 0;
    info->idxStr = sqlite3_mprintf("%s", sql.c_str());
    info->needToFreeIdxStr = true;
    int64_t wildGuess = vtab->InitialRowCount / argvIndex;
    info->estimatedRows = wildGuess;
    info->estimatedCost = (double)wildGuess;
        // wild guess of the effect of each WHERE constraint. we just want to induce sqlite to give us as many
        // constraints as possible for a given query.

    return SQLITE_OK;
}

static int AdoOpen(sqlite3_vtab* pVTab, sqlite3_vtab_cursor** ppCursor) {
#ifdef DEBUG
    System::Diagnostics::Debug::WriteLine("AdoOpen");
#endif
    auto cursor = new AdoCursor;
    cursor->Table = (AdoTable*)pVTab;
    cursor->Connection = cursor->Table->ConnectionCreator->Invoke(cursor->Table->ConnectionString);
    cursor->Connection->Open();
    *ppCursor = &cursor->Super;
    return SQLITE_OK;
}

static int AdoClose(sqlite3_vtab_cursor* pCur) {
#ifdef DEBUG
    System::Diagnostics::Debug::WriteLine("AdoClose");
#endif
    auto cursor = (AdoCursor*)pCur;
    
    IDataReader^ reader = cursor->Reader;
    delete reader;
    cursor->Reader = nullptr;

    IDbCommand^ command = cursor->Command;
    delete command;
    cursor->Command = nullptr;

    IDbConnection^ conn = cursor->Connection;
    delete conn;
    cursor->Connection = nullptr;
    
    delete cursor;
    return SQLITE_OK;
}

static int AdoFilter(sqlite3_vtab_cursor* pCur, int idxNum, const char* idxStr, int argc, sqlite3_value** argv) {
#ifdef DEBUG
    System::Diagnostics::Debug::WriteLine("AdoFilter: " + Util::Str(idxStr));
#endif
    try {
        auto cursor = (AdoCursor*)pCur;
        auto sql = Util::Str(idxStr);
        if ((IDbCommand^)cursor->Command != nullptr) {
            // why does this happen?
            cursor->Command->Cancel();
            delete cursor->Reader;
            delete cursor->Command;
            delete cursor->Connection;
            cursor->Connection = cursor->Table->ConnectionCreator->Invoke(cursor->Table->ConnectionString);
            cursor->Connection->Open();
        }
        auto cmd = cursor->Connection->CreateCommand();
        cmd->CommandText = sql;
        for (int i = 0; i < argc; i++) {
            Object^ argVal = nullptr;
            switch (sqlite3_value_type(argv[i])) {
                case SQLITE_INTEGER:
                    argVal = sqlite3_value_int64(argv[i]);
                    break;
                case SQLITE_FLOAT:
                    argVal = sqlite3_value_double(argv[i]);
                    break;
                case SQLITE_NULL:
                    argVal = DBNull::Value;
                    break;
                case SQLITE_TEXT:
                    argVal = Util::Str((const wchar_t*)sqlite3_value_text16(argv[i]));
                    break;
                default:
                    throw gcnew Exception("Data type not supported.");
            }
            auto varName = "@arg" + (i + 1).ToString();
            auto parameter = cmd->CreateParameter();
            parameter->ParameterName = varName;
            parameter->Value = argVal;
            cmd->Parameters->Add(parameter);
        }
        auto reader = cmd->ExecuteReader();
        cursor->Command = cmd;
        cursor->Reader = reader;
        cursor->IsEof = !cursor->Reader->Read();
        return SQLITE_OK;
    } catch (Exception^) {
        return SQLITE_ERROR;
    }
}

static int AdoNext(sqlite3_vtab_cursor* pCur) {
    auto cursor = (AdoCursor*)pCur;
    cursor->IsEof = !cursor->Reader->Read();
    return SQLITE_OK;
}

static int AdoEof(sqlite3_vtab_cursor* pCur) {
    auto cursor = (AdoCursor*)pCur;
    return cursor->IsEof ? 1 : 0;
}

static void ResultText16(sqlite3_context* ctx, String^ str) {
    auto wstr = Util::WStr(str);
    auto wstrCopy = _wcsdup(wstr.c_str());
    auto lenB = wstr.size() * sizeof(wchar_t);
    sqlite3_result_text16(ctx, wstrCopy, (int)lenB, free);
}

static int AdoColumn(sqlite3_vtab_cursor* pCur, sqlite3_context* ctx, int n) {
    try {
        auto cursor = (AdoCursor*)pCur;
        if (cursor->IsEof) {
            return SQLITE_ERROR;
        }
        auto type = cursor->Reader->GetFieldType(n);
        if (cursor->Reader->IsDBNull(n)) {
            sqlite3_result_null(ctx);
        } else if (type == Int16::typeid) {
            sqlite3_result_int(ctx, cursor->Reader->GetInt16(n));
        } else if (type == Int32::typeid) {
            sqlite3_result_int(ctx, cursor->Reader->GetInt32(n));
        } else if (type == Int64::typeid) {
            sqlite3_result_int64(ctx, cursor->Reader->GetInt64(n));
        } else if (type == Byte::typeid) {
            sqlite3_result_int(ctx, cursor->Reader->GetByte(n));
        } else if (type == Single::typeid) {
            sqlite3_result_double(ctx, cursor->Reader->GetFloat(n));
        } else if (type == Double::typeid) {
            sqlite3_result_double(ctx, cursor->Reader->GetDouble(n));
        } else if (type == Decimal::typeid) {
            sqlite3_result_double(ctx, (double)cursor->Reader->GetDecimal(n));
        } else if (type == String::typeid) {
            ResultText16(ctx, cursor->Reader->GetString(n));
        } else if (type == Char::typeid) {
            ResultText16(ctx, gcnew String(cursor->Reader->GetChar(n), 1));
        } else if (type == Boolean::typeid) {
            sqlite3_result_int(ctx, cursor->Reader->GetBoolean(n) ? 1 : 0);
        } else if (type == NpgsqlTypes::NpgsqlDate::typeid) {
            auto reader = (NpgsqlDataReader^)(IDataReader^)cursor->Reader;
            ResultText16(ctx, ((DateTime)reader->GetDate(n)).ToString("yyyy-MM-dd"));
        } else if (type == NpgsqlTypes::NpgsqlDateTime::typeid || type == DateTime::typeid) {
            ResultText16(ctx, ((DateTime)cursor->Reader->GetDateTime(n)).ToString("yyyy-MM-ddTHH:mm:ss.fffzzz"));
        } else {
            ResultText16(ctx, cursor->Reader->GetValue(n)->ToString());
        }
        return SQLITE_OK;
    } catch (Exception^) {
        return SQLITE_ERROR;
    }
}

static int AdoRowid(sqlite3_vtab_cursor* pCur, sqlite3_int64* pRowid) {
    auto cursor = (AdoCursor*)pCur;
    array<Object^>^ values = gcnew array<Object^>(cursor->Table->ColumnNames->Count);
    cursor->Reader->GetValues(values);
    int64_t hash = 0;
    for each (auto value in values) {
        hash ^= value->GetHashCode();
        hash << 2;
    }
    *pRowid = hash;
    return SQLITE_OK;
}

static int AdoRename(sqlite3_vtab* pVtab, const char* zNew) {
    // don't care
    return SQLITE_OK;
}

static void AdoPopulateModule(sqlite3_module* module) {
    module->iVersion = 1;
    module->xCreate = AdoCreate;
    module->xConnect = AdoCreate;
    module->xBestIndex = AdoBestIndex;
    module->xDisconnect = AdoDestroy;
    module->xDestroy = AdoDestroy;
    module->xOpen = AdoOpen;
    module->xClose = AdoClose;
    module->xFilter = AdoFilter;
    module->xNext = AdoNext;
    module->xEof = AdoEof;
    module->xColumn = AdoColumn;
    module->xRowid = AdoRowid;
    module->xRename = AdoRename;
}

// --- PostgreSQL ---
static sqlite3_module s_pgModule = { 0 };
static AdoCreateInfo s_pgCreateInfo;
static IDbConnection^ PgCreateConnection(String^ connStr) {
    return gcnew NpgsqlConnection(connStr);
}
void Notebook::InstallPgModule() {
    if (s_pgModule.iVersion != 1) {
        AdoPopulateModule(&s_pgModule);
        s_pgCreateInfo.ConnectionCreator = gcnew Func<String^, IDbConnection^>(PgCreateConnection);
    }
    SqliteCall(sqlite3_create_module_v2(_sqlite, "pgsql", &s_pgModule, &s_pgCreateInfo, NULL));
}

// --- Microsoft SQL Server ---
static sqlite3_module s_msModule = { 0 };
static AdoCreateInfo s_msCreateInfo;
static IDbConnection^ MsCreateConnection(String^ connStr) {
    return gcnew SqlConnection(connStr);
}
void Notebook::InstallMsModule() {
    if (s_msModule.iVersion != 1) {
        AdoPopulateModule(&s_msModule);
        s_msCreateInfo.ConnectionCreator = gcnew Func<String^, IDbConnection^>(MsCreateConnection);
    }
    SqliteCall(sqlite3_create_module_v2(_sqlite, "mssql", &s_msModule, &s_msCreateInfo, NULL));
}

// --- MySQL ---
static sqlite3_module s_myModule = { 0 };
static AdoCreateInfo s_myCreateInfo;
static IDbConnection^ MyCreateConnection(String^ connStr) {
    return gcnew MySqlConnection(connStr);
}
void Notebook::InstallMyModule() {
    if (s_myModule.iVersion != 1) {
        AdoPopulateModule(&s_myModule);
        s_myCreateInfo.ConnectionCreator = gcnew Func<String^, IDbConnection^>(MyCreateConnection);
    }
    SqliteCall(sqlite3_create_module_v2(_sqlite, "mysql", &s_myModule, &s_myCreateInfo, NULL));
}