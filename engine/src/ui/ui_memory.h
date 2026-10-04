#pragma once
#include "ui_shared.h"

uint32_t *GetMemory(uint32_t size);
void FreeMemory(char *ptr);
uint8_t *GetClearedMemory(uint32_t size);
token_s *PC_CopyToken(token_s *token);
void PC_FreeToken(token_s *token);
define_s *PC_CopyDefine(source_s *source, define_s *define);
extern int numtokens;

void Statement_AddEntry(statement_s *statement, expressionEntry *entry);
void Statement_AddOperator(statement_s *statement, operationEnum op);
void Statement_AddIntOperand(statement_s *statement, int val);
void Statement_AddFloatOperand(statement_s *statement, float val);
void Statement_AddStringOperand(statement_s *statement, char *str);
