#pragma once

#include <string_view>
#include <string>
#include <functional>

#include "scanner.hpp"
#include "vm.hpp"

struct Parser;
struct ParseRule;
struct Compilation;

struct Compiler;

void errorAt(Parser& parser, const Token token, std::string_view message) noexcept;
void errorAtCurrent(Parser& parser, std::string_view message) noexcept;
void error(Parser& parser, std::string_view message) noexcept;

enum class Precedence : uint8_t
{
    None,
    Assignment,     // =
    Or,             // ||
    And,            // &&
    Equality,       // == !=
    Comparison,     // < > <= >=
    Term,           // + -
    Factor,         // * /
    Unary,          // ! -
    Call,           // . ()
    Primary,
};

using ParseFn = std::function<void(bool canAssign)>;
struct ParseRule
{
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
};


struct Parser
{
    Token current, previous;
    Scanner scanner;
    bool hadError, panicMode;

    Parser() = delete;

    constexpr explicit Parser(std::string_view source) noexcept
        : scanner(source), hadError(false), panicMode(false)
    {}

  
    void advance() noexcept
    {
        // Store the current token.
        previous = current;

        // If valid simply return, else output error and scan next.
        while(true)
        {
            current = scanner.scanToken();
            if (current.type != TokenType::Error) break;
            errorAtCurrent(*this, current.text);
        }
    }

    void consume(TokenType type, std::string_view message) noexcept
    {
        // If the current token's type
        // is the expected tpye,
        // consume it and return.
        if (current.type == type)
        {
            advance();
            return;
        }

        // Type wasn't expected, output error.
        errorAtCurrent(*this, message);
    }

    // Check the current token's type
    // returns true if it is of expected type.
    [[nodiscard]] bool check(TokenType type) const noexcept
    {
        return current.type == type;
    }

};


void errorAt(Parser& parser, const Token token, std::string_view message) noexcept
{
    if (parser.panicMode) return;
    parser.panicMode = true;

    std::cerr << "[line " << token.line << "] Error";

    if (token.type == TokenType::Eof)
    {
        std::cerr << " at end";
    }
    else if (token.type == TokenType::Error)
    {
        // Nothing
    }
    else
    {
        std::cerr << " at " << token.text;
    }
    std::cerr << ": " << message << '\n';
    parser.hadError = true;
}

void errorAtCurrent(Parser& parser, std::string_view message) noexcept
{
    errorAt(parser, parser.current, message);
}

void error(Parser& parser, std::string_view message) noexcept
{
    errorAt(parser, parser.previous, message);
}


// Used for local variables.
// - We store the name of the variable
// 
// - When resolving an identifer, we 
//   compare the identifier' lexeme
//   with each local's name to find
//   a match.
// 
// - The depth field records the scope 
//   depth of the block where the local
//   variable was declared.
struct Local
{   
    std::string_view    name;   // Name of local
    int                 depth;  // The depth level of local
};

// The compiler will help us resolve local variables.
struct Compiler
{
    std::array<Local, UINT8_COUNT>  locals;
    int                             localCount = 0;
    size_t                          scopeDepth = 0;
};

// This is the struct containing
// all the functions which will 
// used to compile the source code.
struct Compilation
{
    Chunk* compilingChunk = nullptr;

    Compiler                 compiler;          // Implicit default constructed.
    std::unique_ptr<Parser>  parser = nullptr;  // 

    [[nodiscard]] Chunk& currentChunk() noexcept { return *compilingChunk; }

     // Write an OpCode byte to the chunk.
     void emitByte(OpCode byte)
     {
        currentChunk().write(byte, parser->previous.line);
     }

    // Write a byte to the chunk.
    void emitByte(uint8_t byte)
    {
        currentChunk().write(byte, parser->previous.line);
    }

    // Write the OpCode for return to the chunk.
    void emitReturn()
    {
        emitByte(OpCode::RETURN);
    }

    // Write two bytes to the chunk.
    void emitByte(uint8_t byte1, uint8_t byte2)
    {
        emitByte(byte1);
        emitByte(byte2);
    }

    // Write two OpCode bytes to the chunk.
    void emitByte(OpCode byte1, OpCode byte2)
    {
        emitByte(byte1);
        emitByte(byte2);
    }

    bool compile(std::string_view code, Chunk& chunk) 
    {
        // Setup
        this->parser = std::make_unique<Parser>(code);
        compilingChunk = &chunk;

        // Compiling
        parser->advance();


        while (!match(TokenType::Eof))
        {
            declaration();
        }
        
        endCompiler();
        return !parser->hadError;
    }

    void endCompiler() noexcept
    {
        emitReturn();
        #ifdef DEBUG_PRINT_CODE
        if (!parser->hadError) 
        {
            currentChunk().disassembleChunk("code");
        }
        #endif
    }

    // Error sychronization -
    // If we hit a compile-error parsing the
    // previous statement, we enter panic mode.
    // When that happens, after the statement 
    // we start synchronizing.
    void synchronize()
    {
        // Reset flag.
        parser->panicMode = false;

        // Skip tokens indiscriminately, until we reach
        // something that looks like a statement boundary.
        // Like a preceding semi-colon (;) or subsquent
        // token which begins a new statement, usually
        // a control flow or declaration keywords.
        while (parser->current.type != TokenType::Eof)
        {
            // Preceding semi-colon.
            if (parser->previous.type == TokenType::Semicolon) return;

            // If it is one of the keywords listed, we stop.
            switch(parser->current.type)
            {
                case TokenType::Class:
                case TokenType::Fun:
                case TokenType::Var:
                case TokenType::For:
                case TokenType::If:
                case TokenType::While:
                case TokenType::Print:
                case TokenType::Return:
                    return;
                default:
                    ;   // Nothing
            }
            
            // No conditions met, keep advancing.
            parser->advance();
        }
    }

    // Parse expression.
    void expression()
    {
        // Beginning of the Pratt parser.

        // Parse with lowest precedence first.
        parsePrecedence(Precedence::Assignment);
    }

    // Variable declaration (A 'var' token found)
    void varDeclaration()
    {
        // Parse the variable name and get back
        // the index of the newly pushed constant (the name)
        auto global = parseVariable("Expect variable name.");

        // We expect the next token to be
        // an assignment operator.
        if (match(TokenType::Equal))
        {
            // If there is an equal token,
            // consume it then evaluate
            // the following expression.
            // The result of the evaluation
            // will be the assigned value
            expression();
        }
        else // No equal token found. We imply unintialized declaration.
        {
            // The expression is declared,
            // but uninitialized, implicitly
            // init to nil.
            emitByte(OpCode::NIL);
        }

        // We expect statements to be 
        // terminated with a semi-colon.
        // Consume the final token to 
        // finalize the statement.
        parser->consume(TokenType::Semicolon, "Expect ';' after variable declaration.");

        // If everything went well then we 
        // can now just define the variable.
        defineVariable(global);
    }

    void expressionStatement()
    {
        expression();
        parser->consume(TokenType::Semicolon, "Expect ';' after expression.");
        emitByte(OpCode::POP);
    }

    // Declaring statements or variables.
    void declaration()
    {
        // Match variable token.
        if (match(TokenType::Var))
        {
            // After the var token is consumed, we need
            // to parse for the variable name and value
            varDeclaration();
        }
        else 
        {
            // If it isn't a variable
            // it must be a statement.
            statement();
        }
        
        // Synchronize error after compile-error
        if (parser->panicMode) synchronize();
    }


    void block()
    {
        // While we we haven't reached reached the end of
        // the block, or reach the end, we parse the
        // declaration(s).
        while (!parser->check(TokenType::RightBrace) && !parser->check(TokenType::Eof))
        {
            declaration();
        }

        // The while loop ends when the
        // current token is the right brace
        // (or end of file).

        // We simply consume the right brace
        // to complete the process.
        parser->consume(TokenType::RightBrace, "Expect '}': no matching token found.");

    }

    // Parse statements.
    void statement()
    {
        // Check if we match a print token,
        // if we are then the token will be
        // consumed, then we evaluate the
        // subsequent tokens, expecting them
        // to be expression statements.
        if (match(TokenType::Print))
        {
            printStatement();
        }
        else if (match(TokenType::LeftBrace))
        {
            beginScope();
            block();
            endScope();
        }
        // We're not looking at print,
        // we must be looking at an
        // expression statement.
        else
        {
            expressionStatement();
        }
    }

    // Parse a print statement.
    void printStatement() noexcept
    {
        // Evalaute the expression
        expression();

        // If parsing and evaluating the expression
        // succeeded, we can then consume the ';'
        // concluding the process.
        parser->consume(TokenType::Semicolon, "Expected ';' after value.");

        // If everything succeeded, 
        // simply emit the bytecode
        // for print.
        emitByte(OpCode::PRINT);
    }
    
    // Check if current token matches the current token,
    // if it does, consume it.
    [[nodiscard]] bool match(TokenType type) noexcept
    {
        // If the current token is not the expected type
        // return false.
        if (!parser->check(type)) return false;
        
        // If it was expected, consume it.
        parser->advance();
        return true;
    }

    void number(bool) noexcept
    {
        Value value = std::stod(std::string(parser->previous.text));
        emitConstant(value);
    }

    void emitConstant(Value value) noexcept
    {
        emitByte(static_cast<uint8_t>(OpCode::CONSTANT), makeConstant(value));
    }

    // Create a new constant and add it to the chunk.
    [[nodiscard]] uint8_t makeConstant(Value value) noexcept
    {
        // Add the constant to the current chunk and retrieve the 
        // byte code which corresponds to it.
        auto constant = static_cast<uint8_t>(currentChunk().addConstant(value));

        if (constant > UINT8_MAX)
        {
            error(*parser.get(), "Too many constants in one chunk");
            return 0;
        }

        // Return the bytecode which correspond to 
        // the constant pushed onto the chunk.
        return constant;
    }

    void grouping(bool) noexcept
    {
        expression();
        parser->consume(TokenType::RightParen, "Expect ')' after expression.");
    }

    void unary(bool)
    {
        // Remember the operator
        auto operatorType = parser->previous.type;

        // Compile the operand
        parsePrecedence(Precedence::Unary);

        // Emit the operator instruction
        switch(operatorType)
        {
            case TokenType::Minus:  emitByte(OpCode::NEGATE); break;
            case TokenType::Bang:   emitByte(OpCode::NOT); break;
            default: return; // Unreachable
        }
    }

    void binary(bool)
    {
        // Remember the operator
        auto operatorType = parser->previous.type;

        auto rule = getRule(operatorType);
        parsePrecedence(Precedence(static_cast<int>(rule.precedence) + 1));

        // Emit the corresponding opcode
        switch (operatorType)
        {
            case TokenType::BangEqual:      emitByte(OpCode::EQUAL, OpCode::NOT); break;
            case TokenType::EqualEqual:     emitByte(OpCode::EQUAL); break;
            case TokenType::Greater:        emitByte(OpCode::GREATER); break;
            case TokenType::GreaterEqual:   emitByte(OpCode::LESS, OpCode::NOT); break;
            case TokenType::Less:           emitByte(OpCode::LESS); break;
            case TokenType::LessEqual:      emitByte(OpCode::GREATER, OpCode::NOT); break;

            case TokenType::Plus:           emitByte(OpCode::ADD); break;
            case TokenType::Minus:          emitByte(OpCode::SUBTRACT); break;
            case TokenType::Star:           emitByte(OpCode::MULTIPLY); break;
            case TokenType::Slash:          emitByte(OpCode::DIVIDE); break;
            default: return; // Unreachable
        }
    }

    void literal(bool)
    {
        switch(parser->previous.type)
        {
            case TokenType::False: emitByte(OpCode::FALSE); break;
            case TokenType::Nil: emitByte(OpCode::NIL); break;
            case TokenType::True: emitByte(OpCode::TRUE); break;
            default: return; // Unreachable
        }
    }

    void string(bool)
    {
        // Retrive the text in the form: "str"
        auto str = parser->previous.text;

        // Get rid of quotation marks
        str.remove_prefix(1);
        str.remove_suffix(1);

        // Construct string object
        emitConstant(std::string(str));
    }

    void variable(bool canAssign)
    {
        namedVariable(canAssign);
    }

    [[nodiscard]] int resolveLocal() const
    {
        // Walk the list from the back,
        // returns the first local which
        // has the same name as the identifier
        // token.

        // The list is walked backward, starting from the
        // current deepest layer, because all locals 
        // only have access to local variables declared in
        // lower or equal depth.

        // The compiler's local array will mirror the vm's stack
        // which means that the index can be directly grabbed from
        // here.
        for (auto i = compiler.localCount - 1; i >= 0; i--)
        {
            auto& local = compiler.locals.at(i);
            if (identifiersEqual(parser->previous.text, local.name))
            {
                if (local.depth == -1)
                {
                    error(*parser, "Can't read local variable in its own initializer.");
                }
                return i;
            }
        }

        // Variable with given name not found,
        // assumed to be global variable instead.
        return -1;
    }


    void namedVariable(bool canAssign)
    {
        OpCode getOp, setOp;
        int arg = resolveLocal();
        if (arg != -1)
        {
            getOp = OpCode::GET_LOCAL;
            setOp = OpCode::SET_LOCAL;
        }
        else
        {
            arg = identifierConstant();
            getOp = OpCode::GET_GLOBAL;
            setOp = OpCode::SET_GLOBAL;
        }

        // Indiciates that the variable is
        // calling for a setter/ assignment.
        if (canAssign && match(TokenType::Equal))
        {
            // Evaluate the expression (on the right).
            expression();

            // Link variable name to it in the map.
            emitByte(static_cast<uint8_t>(setOp), static_cast<uint8_t>(arg));
        }
        else
        {
            // Calls for getter / access.
            emitByte(static_cast<uint8_t>(getOp), static_cast<uint8_t>(arg));
        }
    }

    // Precedence
    void parsePrecedence(Precedence precedence)
    {
        // Consume the first token.
        parser->advance();
        
        // Get the type of the token.
        auto type = parser->previous.type;

        // Get the precedence rule which applies 
        // to the given token.
        auto prefixRule = getRule(type).prefix;

        if (prefixRule == nullptr)
        {
            error(*parser, "Expected expression.");
            return;
        }

        // Invoke function
        bool canAssign = precedence <= Precedence::Assignment;
        prefixRule(canAssign);

        while (precedence <= getRule(parser->current.type).precedence)
        {
            parser->advance();
            ParseFn infixRule = getRule(parser->previous.type).infix;
            infixRule(canAssign);
        }

        if (canAssign && match(TokenType::Equal))
        {
            error(*this->parser, "Invalid assignment target.");
        }
    }

    // Create a new value with the previous token's lexeme
    // and return the index at which is is added in the constant table.
    [[nodiscard]] uint8_t identifierConstant() noexcept
    {
        return makeConstant({std::string(parser->previous.text)});
    }

    [[nodiscard]] bool identifiersEqual(std::string_view str1, std::string_view str2) const noexcept
    {
        return str1 == str2;
    }

    void declareVariable()
    {
        // If we are in global scope return.
        // This is only for local variables.
        if (compiler.scopeDepth == 0) return;

        auto name = parser->previous.text;

        for (auto i = compiler.localCount - 1; i >= 0; i--)
        {
            assert(i < compiler.locals.size());
            auto& local = compiler.locals[i];
            if (local.depth != -1 && local.depth < compiler.scopeDepth)
            {
                break;
            }

            if (identifiersEqual(name, local.name))
            {
                error(*parser, "Re-definition of an existing variable in this scope.");
            }
        }

        // Add the local variable to the compiler.
        // This makes sure the compiler keeps track
        // of the existence of the variable.
        addLocal(name , *parser.get());
    }

    // Parses the variable's name.
    [[nodiscard]] uint8_t parseVariable(std::string_view message) noexcept
    {
        // We expect the token after 'var' to be an identifer.
        parser->consume(TokenType::Identifier, message);
        
        // Declare the variable
        declareVariable();

        // Check if we are in scope (Not in global)
        // At runtime, locals aren't looked up by name,
        // meaning that there is no need to stuff them
        // int the constant table, if declaration is in 
        // scope, we just return a dummy table index.
        if (compiler.scopeDepth > 0) return 0;

        // If we made it here, it meant that we successfully
        // consumed an identifer token. We now want to add
        // the token lexeme as a new constant, then return
        // the index that it was added at the constant table.
        return identifierConstant();
    }

    // Mark the latest variable initialized's scope to
    // the current scope.
    void markInitialized()
    {
        compiler.locals[compiler.localCount - 1].depth = static_cast<int>(compiler.scopeDepth);
    }

    // Define the variable.
    // Global refers to the index of the name
    // in the chunk's constant collection.
    void defineVariable(uint8_t global)
    {
        // If we are in a scope, we do not want to define global.
        if (compiler.scopeDepth > 0) 
        {
            markInitialized();
            return;
        }
        
        // emit the OpCode and the index of the name. (in chunk's constants)
        emitByte(static_cast<uint8_t>(OpCode::DEFINE_GLOBAL), global);
    }

    // By incrementing the
    // depth, we declare that 
    // a new block has begun.
    void beginScope() noexcept
    {
        compiler.scopeDepth++;
    }

    // By decrementing the 
    // depth, we declare that
    // a block is out of scope,
    // so we simply return to
    // the previous layer.
    void endScope() noexcept
    {
        // End of scope.
        // We go back one scope.
        compiler.scopeDepth--;

        // Pop all the variables which are now out of scope.
        // The while loop crawls backwards on locals and
        // keeps popping the variables off the stack until
        // it reaches a local variable which has the same 
        // depth as the current depth being evaluated.
        
        // This works beautifully thanks to the fact that
        // variables in the 'locals' array are nicely grouped 
        // together, and that the depth attribute is incrementing
        // uniformly with each subsequent group.

        /* i.e:
                                                                                                                       
            var greeting;
            var message;
            {
                var firstName;           ===     [{greeting, 0}, {message, 0}, {firstName, 1}, {middleName, 1}, {lastName, 1}, {deepestLevel, 2}]
                var middleName;          ===       |_______________________|   |                                            |  |                |
                var lastName;            ===                   0               |____________________________________________|  |                |
                {                                                                                     1                        |________________|
                    var deepestLevel;                                                                                                  2
                }
            }
        */

        // Check that variable count isn't 0.
        while (compiler.localCount > 0 
            // Check that the current target variable has deeper depth than the current
            // deepest scope.
            && compiler.locals[compiler.localCount - 1].depth > compiler.scopeDepth)
        {
            // Pop the value off the stack.
            emitByte(OpCode::POP);

            // One less variable.
            compiler.localCount--;
        }

       /* for (auto slot = 0; slot < compiler.localCount; ++slot)
        {
            std::cout << "{ " << compiler.locals[slot].name << " }";
        }
        std::cout << '\n';*/
    }

    // Add local variable to the compiler.
    void addLocal(std::string_view name, Parser& targetParser)
    {

        // Since our indexs are stored in a single byte,
        // it means that we can only support 256 local
        // variables in scope at one time, so it must be
        // prevented.
        if (compiler.localCount == UINT8_COUNT)
        {
            error(targetParser, "Too many local variables declared in function.");
            return;
        }

        assert(compiler.localCount < compiler.locals.size());
        auto& local = compiler.locals[compiler.localCount++];
        local.name = name;
        local.depth = -1;

        /*for (auto slot = 0; slot < compiler.localCount; ++slot)
        {
            std::cout << "{ " << compiler.locals[slot].name << " }";
        }
        std::cout << '\n';*/
    }

    // get rule
    [[nodiscard]] const ParseRule& getRule(TokenType type) noexcept
    {
        auto grouping = [this](bool canAssign) { this->grouping(canAssign); };
        auto unary = [this](bool canAssign) { this->unary(canAssign); };
        auto binary = [this](bool canAssign) { this->binary(canAssign); };
        auto number = [this](bool canAssign) { this->number(canAssign); };
        auto literal = [this](bool canAssign) { this->literal(canAssign); };
        auto string = [this](bool canAssign) { this->string(canAssign); };
        auto variable = [this](bool canAssign) { this->variable(canAssign); };
        
        static ParseRule rls[] = 
        {
            {grouping,       nullptr,    Precedence::None},     // TokenType::LEFT_PAREN
            {nullptr,       nullptr,    Precedence::None},      // TokenType::RIGHT_PAREN
            {nullptr,       nullptr,    Precedence::None},      // TokenType::LEFT_BRACE
            {nullptr,       nullptr,    Precedence::None},      // TokenType::RIGHT_BRACE
            {nullptr,       nullptr,    Precedence::None},      // TokenType::COMMA
            {nullptr,       nullptr,    Precedence::None},      // TokenType::DOT
            {unary,         binary,     Precedence::Term},      // TokenType::MINUS
            {nullptr,       binary,     Precedence::Term},      // TokenType::PLUS
            {nullptr,       nullptr,    Precedence::None},      // TokenType::SEMICOLON
            {nullptr,       binary,     Precedence::Factor},    // TokenType::SLASH
            {nullptr,       binary,     Precedence::Factor},    // TokenType::STAR
            {unary,         nullptr,    Precedence::None},      // TokenType::BANG
            {nullptr,       binary,     Precedence::Equality},  // TokenType::BANG_EQUAL
            {nullptr,       nullptr,    Precedence::None},      // TokenType::EQUAL
            {nullptr,       binary,     Precedence::Equality},  // TokenType::EQUAL_EQUAL
            {nullptr,       binary,     Precedence::Comparison},// TokenType::GREATER
            {nullptr,       binary,     Precedence::Comparison},// TokenType::GREATER_EQUAL
            {nullptr,       binary,     Precedence::Comparison},// TokenType::LESS
            {nullptr,       binary,     Precedence::Comparison},// TokenType::LESS_EQUAL
            {variable,      nullptr,    Precedence::None},      // TokenType::IDENTIFIER
            {string,        nullptr,    Precedence::None},      // TokenType::STRING
            {number,        nullptr,    Precedence::None},      // TokenType::NUMBER
            {nullptr,       nullptr,    Precedence::None},      // TokenType::AND
            {nullptr,       nullptr,    Precedence::None},      // TokenType::CLASS
            {nullptr,       nullptr,    Precedence::None},      // TokenType::ELSE
            {literal,       nullptr,    Precedence::None},      // TokenType::FALSE
            {nullptr,       nullptr,    Precedence::None},      // TokenType::FOR
            {nullptr,       nullptr,    Precedence::None},      // TokenType::FUN
            {nullptr,       nullptr,    Precedence::None},      // TokenType::IF
            {literal,       nullptr,    Precedence::None},      // TokenType::NIL
            {nullptr,       nullptr,    Precedence::None},      // TokenType::OR
            {nullptr,       nullptr,    Precedence::None},      // TokenType::PRINT
            {nullptr,       nullptr,    Precedence::None},      // TokenType::RETURN
            {nullptr,       nullptr,    Precedence::None},      // TokenType::SUPER
            {nullptr,       nullptr,    Precedence::None},      // TokenType::THIS
            {literal,       nullptr,    Precedence::None},      // TokenType::TRUE
            {nullptr,       nullptr,    Precedence::None},      // TokenType::VAR
            {nullptr,       nullptr,    Precedence::None},      // TokenType::WHILE
            {nullptr,       nullptr,    Precedence::None},      // TokenType::ERROR
            {nullptr,       nullptr,    Precedence::None},      // TokenType::EOF
        };

        return rls[static_cast<int>(type)];
    }
};




