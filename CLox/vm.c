#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "chunk.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"

VM vm;

// Begin Native Functions Section
static Value clockNative(int argCount, Value* args) {
	return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value readNumNative(int argCount, Value* args) {
	int number;
	scanf_s("%d", &number);
	return NUMBER_VAL(number);
}

static Value readStrNative(int argCount, Value* args) {
	char str[100];
	scanf_s("%[^\n]", str, (unsigned int)sizeof(str));		
	
	return OBJ_VAL(copyString(str,100));
}

// End Native Functions Section

void resetStack() {
	vm.stackTop = vm.stack;
	vm.frameCount = 0;
}

static void runtimeError(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(stderr, format, args);
	va_end(args);
	fputs("\n", stderr);

	for (int i = vm.frameCount - 1; i > 0; i--) {
		CallFrame* frame = &vm.frames[i];
		ObjFunction* function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		fprintf(stderr, "[line %d] in ",
			function->chunk.lines[instruction]);
		if (function->name == NULL) {
			fprintf(stderr, "script\n");
		}
		else {
			fprintf(stderr, "%s()\n", function->name->chars);
		}
	}


	resetStack();
	vm.objects = NULL;
}

static void defineNative(const char* name, NativeFn function) {
	push(OBJ_VAL(copyString(name, (int)strlen(name))));
	push(OBJ_VAL(newNative(function)));
	tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
	pop();
	pop();
}

void initVM() {
	vm.stackTop = vm.stack;
	resetStack();
	vm.objects = NULL;
	initTable(&vm.globals);
	initTable(&vm.strings);

	// initialization of native functions
	defineNative("clock", clockNative);
	defineNative("readNum", readNumNative);
	defineNative("readStr", readStrNative);
}

void freeVM() {
	freeTable(&vm.globals);
	freeTable(&vm.strings);
	freeObjects();
}

void push(Value value) {
	*vm.stackTop = value;
	vm.stackTop++;
}

Value pop() {
	vm.stackTop--;
	return *vm.stackTop;
}

static Value peek(int distance) {
	return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
	if (argCount != closure->function->arity) {
		runtimeError("Expected %d arguments but got %d.", closure->function->arity, argCount);
		return false;
	}

	if (vm.frameCount == FRAMES_MAX) {
		runtimeError("Stack overflow.");
		return false;
	}

	CallFrame* frame = &vm.frames[vm.frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = vm.stackTop - argCount - 1;
	return true;
}

static bool callValue(Value callee, int argCount) {
	if (IS_OBJ(callee)) {
		switch (OBJ_TYPE(callee)) {
			case OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount);
			case OBJ_NATIVE:
				NativeFn native = AS_NATIVE(callee);
				Value result = native(argCount, vm.stackTop - argCount);
				vm.stackTop -= argCount + 1;
				push(result);
				return true;
			default:
				break;
		}
	}
	runtimeError("Can only call functions and classes.");
	return false;
}

static bool isFalsey(Value value) {
	return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
	ObjString* b = AS_STRING(pop());
	ObjString* a = AS_STRING(pop());

	int length = a->length + b->length;

	char* chars = ALLOCATE(char, length + 1);
	memcpy(chars, a->chars, a->length);
	memcpy(chars + a->length, b->chars, b->length);
	chars[length] = '\0';

	ObjString* result = takeString(chars, length);
	push(OBJ_VAL(result));
}

static InterpretResult run() {
	CallFrame* frame = &vm.frames[vm.frameCount - 1];

	#define READ_BYTE() (*frame->ip++)
	#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
	#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
	#define READ_STRING() AS_STRING(READ_CONSTANT())
	// This do while block gives us a way to contain multiple statements inside a block that also permits a semicolon at the end
	#define BINARY_OP(valueType, op) do { if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; }  double b = AS_NUMBER(pop()); double a = AS_NUMBER(pop()); push(valueType(a op b)); } while(false)
	for (;;) {
	#ifdef DEBUG_TRACE_EXECUTION
		printf("          ");
		for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
			printf("[ ");
			printValue(*slot);
			printf(" ]");
		}
		printf("\n");
		disassembleInstruction(&frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code));
	#endif // !DEBUG_TRACE_EXECUTION

		uint8_t instruction;
		switch (instruction = READ_BYTE()) {
			case OP_CONSTANT: {
				Value constant = READ_CONSTANT();
				push(constant);
				break;
			}
			case OP_NIL: push(NIL_VAL); break;
			case OP_TRUE: push(BOOL_VAL(true)); break;
			case OP_FALSE: push(BOOL_VAL(false)); break;
			case OP_POP: pop(); break;
			case OP_GET_LOCAL: {
				uint8_t slot = READ_BYTE();
				push(frame->slots[slot]);
				break;
			}
			case OP_SET_LOCAL: {
				uint8_t slot = READ_BYTE();
				frame->slots[slot] = peek(0);
				break;
			}
			case OP_GET_GLOBAL: {
				ObjString* name = READ_STRING();
				Value value;
				if (!tableGet(&vm.globals, name, &value)) {
					runtimeError("Undefined variable '%s'.", name->chars);
					return INTERPRET_RUNTIME_ERROR;
				}
				push(value);
				break;
			}
			case OP_DEFINE_GLOBAL: {
				ObjString* name = READ_STRING();
				tableSet(&vm.globals, name, peek(0));
				pop();
				break;
			}
			case OP_SET_GLOBAL: {
				ObjString* name = READ_STRING();
				if (tableSet(&vm.globals, name, peek(0))) {
					tableDelete(&vm.globals, name);
					runtimeError("Undefined variable '%s'.", name->chars);
					return INTERPRET_RUNTIME_ERROR;
				}
				break;
			}
			case OP_EQUAL: {
				Value a = pop();
				Value b = pop();
				push(BOOL_VAL(valuesEqual(a, b)));
				break;
			}
			case OP_GREATER: 
				BINARY_OP(BOOL_VAL, >);
				break;
			case OP_LESS:
				BINARY_OP(BOOL_VAL, <);
				break;
			case OP_ADD: {
				if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
					concatenate();
				}
				else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
					double b = AS_NUMBER(pop());
					double a = AS_NUMBER(pop());
					push(NUMBER_VAL(a + b));
				}
				else {
					runtimeError("Operands must be two numbers or two strings");
					return INTERPRET_RUNTIME_ERROR;
				}
				break;
			}				
			case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
			case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
			case OP_DIVIDE: BINARY_OP(NUMBER_VAL, /); break;
			case OP_NOT:
				push(BOOL_VAL(isFalsey(pop())));
				break;
			case OP_NEGATE: 
				if (!IS_NUMBER(peek(0))) {
					runtimeError("Operand must be a number.");
					return INTERPRET_RUNTIME_ERROR;
				}
				push(NUMBER_VAL(-AS_NUMBER(pop())));
				break;
			case OP_PRINT: {
				printValue(pop());
				printf("\n");
				break;
			}
			case OP_JUMP: {
				uint16_t offset = READ_SHORT();
				frame->ip += offset;
				break;
			}
			case OP_JUMP_IF_FALSE: {
				uint16_t offset = READ_SHORT();
				if (isFalsey(peek(0))) frame->ip += offset;
				break;
			}
			case OP_LOOP: {
				uint16_t offset = READ_SHORT();
				frame->ip -= offset;
				break;
			}
			case OP_CALL: {
				int argCount = READ_BYTE();
				if (!callValue(peek(argCount), argCount)) {
					return INTERPRET_RUNTIME_ERROR;
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;	
			}
			case OP_CLOSURE: {
				ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
				ObjClosure* closure = newClosure(function);
				push(OBJ_VAL(closure));
				break;
			}
			case OP_RETURN: {				
				Value result = pop();
				vm.frameCount--;
				if (vm.frameCount == 0) {
					// once it is the vary last CallFrame, it means the entire program is done
					// pop the main script function from the stack and then exit 
					pop(); 
					return INTERPRET_OK;
				}

				// the top of the stack ends up right at the beginning of the returning function's stack window
				vm.stackTop = frame->slots;
				// put the result of the function onto the stack into a lower location
				push(result);
				frame = &vm.frames[vm.frameCount - 1];
				return INTERPRET_OK;
			}
		}
	}
	#undef READ_BYTE
	#undef READ_SHORT
	#undef READ_CONSTANT
	#undef READ_STRING
	#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
	ObjFunction* function = compile(source);
	if (function == NULL) return INTERPRET_COMPILER_ERROR;

	/*
	 * The code below looks a little silly because we still
	 * push the original ObjFunction onto the stack.
	 * Then we pop it after creating the closure,
	 * only to then push the closure. Why put the
	 * ObjFunction on there at all? As usual, when
	 * you see weird stack stuff going on, it's to keep
	 * the garbage collector aware of some heap allocated objects.
	 */
	push(OBJ_VAL(function));
	ObjClosure* closure = newClosure(function);
	pop();
	push(OBJ_VAL(closure));
	call(closure, 0);
	
	return run();
}