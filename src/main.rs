use std::{
    io::{self, BufRead, Write},
    process::ExitCode,
};

use clap::{command, Parser};

//
// CLI.
//

#[derive(Parser, Debug)]
#[command(version, about,long_about = None)]
struct Args {
    // Source code file path. If not specifed, REPL mode will start.
    #[arg(short, long)]
    path: Option<String>,
}

use log::error;
use num_derive::FromPrimitive;
use num_traits::FromPrimitive;

//
// Value.
//
type Value = f32;

pub fn print_value(value: &Value) {
    print!("{}", value);
}

//
// Chunk.
//

// List of VM instructions.
#[derive(FromPrimitive)]
#[repr(u8)]
pub enum Opcode {
    Constant = 1,
    Add,
    Subtract,
    Multiply,
    Divide,
    Negate,
    Return,
}

/// A chunk is a sequence of bytecode.
pub struct Chunk {
    /// The list of bytecode which represents the program.
    pub code: Vec<u8>,
    /// The list of constants declared.
    pub constants: Vec<Value>,
    /// The line numbers for each bytecode.
    pub lines: Vec<i32>,
}

impl Chunk {
    /// Returns a newly intialized chunk.
    pub fn new() -> Self {
        Self {
            code: vec![],
            constants: vec![],
            lines: vec![],
        }
    }

    /// Write a byte into the chunk.
    pub fn write(&mut self, byte: u8, line: i32) {
        self.code.push(byte);
        self.lines.push(line);
    }

    /// Write an instruction into the chunk.
    pub fn write_instruction(&mut self, instruction: Opcode, line: i32) {
        self.write(instruction as u8, line);
    }

    // TODO: I should probably move this out.
    /// Print instruction name and return the next offset.
    pub fn simple_instruction(&self, name: &str, offset: usize) -> usize {
        println!("{} ", name);
        offset + 1
    }

    /// Push a constant into the constant vector, return the index which the constant resides.
    pub fn add_constant(&mut self, value: Value) -> u8 {
        self.constants.push(value);
        (self.constants.len() - 1) as u8
    }

    /// Print the constant's handle and it's value. Returns the next offset.
    pub fn constant_instruction(&self, name: &str, offset: usize) -> usize {
        let constant_index = self.code[(offset + 1) as usize] as usize;
        print!("{:-16} {:4} '", name, constant_index);
        print_value(&self.constants[constant_index]);
        println!("'");
        return offset + 2;
    }

    /// Dump the instruction's information.
    pub fn disassemble_instruction(&self, offset: usize) -> usize {
        print!("{:04} ", offset);

        let offset_index = offset;
        if offset_index > 0 && self.lines[offset_index] == self.lines[offset_index - 1] {
            print!("   | ")
        } else {
            print!("{:4} ", self.lines[offset_index]);
        }

        let byte = self.code[offset as usize];
        let instruction: Option<Opcode> = FromPrimitive::from_u8(byte);

        match instruction {
            Some(Opcode::Constant) => self.constant_instruction("OP_CONSTANT", offset),
            Some(Opcode::Add) => self.simple_instruction("OP_CONSTANT", offset),
            Some(Opcode::Subtract) => self.simple_instruction("OP_SUBTRACT", offset),
            Some(Opcode::Multiply) => self.simple_instruction("OP_MULTIPLY", offset),
            Some(Opcode::Divide) => self.simple_instruction("OP_DIVIDE", offset),
            Some(Opcode::Negate) => self.simple_instruction("OP_NEGATE", offset),
            Some(Opcode::Return) => self.simple_instruction("OP_RETURN", offset),
            None => {
                println!("Unknown opcode {}", byte);
                offset + 1
            }
        }
    }

    /// For debugging. Dumps the program's instructions.
    pub fn disassemble_chunk(&self, name: &str) {
        println!("== {} ==", name);

        let mut offset: usize = 0;
        while offset < self.code.len() {
            offset = self.disassemble_instruction(offset);
        }
    }
}

//
// Token.
//
#[derive(Clone, PartialEq, Eq)]
enum TokenKind {
    // Single-character tokens.
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    Comma,
    Dot,
    Minus,
    Plus,
    Semicolon,
    Slash,
    Star,
    // One or two character tokens.
    Bang,
    BangEqual,
    Equal,
    EqualEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    // Literals.
    Identifier,
    String,
    Number,
    // Keywords.
    And,
    Class,
    Else,
    False,
    For,
    Fun,
    If,
    Nil,
    Or,
    Print,
    Return,
    Super,
    This,
    True,
    Var,
    While,

    Error,
    Eof,
}

struct Token<'a> {
    kind: TokenKind,
    start: usize,
    length: usize,
    line: usize,
    source: &'a str,
}

impl<'a> Token<'a> {
    fn lexeme(&'a self) -> &'a str {
        &self.source[self.start..self.start + self.length]
    }

    fn new(tty: TokenKind, start: usize, length: usize, line: usize, source: &'a str) -> Self {
        Self {
            kind: tty,
            start,
            length,
            line,
            source,
        }
    }
}

//
// Scanner.
//
struct Scanner<'a> {
    current: usize,
    line: usize,
    source: &'a str,
    start: usize,
}

impl<'a> Scanner<'a> {
    // Creates a new [`Scanner`].
    fn new(source: &'a str) -> Self {
        Self {
            current: 0,
            line: 1,
            start: 0,
            source,
        }
    }

    fn skip_whitespace(&mut self) {
        loop {
            let c: char = self.peek();
            match c {
                // General whitespace.
                ' ' | '\r' | '\t' => {
                    self.advance();
                }
                // Handle newline.
                '\n' => {
                    self.line += 1;
                    self.advance();
                }
                // Handle comments.
                '/' => {
                    if self.peek_offset(1) == '/' {
                        // Ignore until newline or scanner exhausted.
                        while self.peek() != '\n' && !self.is_at_end() {
                            self.advance();
                        }
                    }
                }
                _ => return,
            }
        }
    }

    // Scan the next token.
    fn scan_token(&mut self) -> Token {
        self.skip_whitespace();
        self.start = self.current;

        if self.is_at_end() {
            return self.make_token(TokenKind::Eof);
        }

        let c = self.advance();

        if c.is_ascii_digit() {
            return self.number();
        } else if c.is_ascii_alphabetic() || c == '_' {
            return self.identifer();
        }

        match c {
            '(' => return self.make_token(TokenKind::LeftParen),
            ')' => return self.make_token(TokenKind::RightParen),
            '{' => return self.make_token(TokenKind::LeftBrace),
            '}' => return self.make_token(TokenKind::RightBrace),
            ';' => return self.make_token(TokenKind::Semicolon),
            ',' => return self.make_token(TokenKind::Comma),
            '.' => return self.make_token(TokenKind::Dot),
            '-' => return self.make_token(TokenKind::Minus),
            '+' => return self.make_token(TokenKind::Plus),
            '/' => return self.make_token(TokenKind::Slash),
            '*' => return self.make_token(TokenKind::Star),
            '!' => {
                if self.match_char('=') {
                    return self.make_token(TokenKind::BangEqual);
                } else {
                    return self.make_token(TokenKind::Bang);
                }
            }
            '=' => {
                if self.match_char('=') {
                    return self.make_token(TokenKind::EqualEqual);
                } else {
                    return self.make_token(TokenKind::Equal);
                }
            }
            '<' => {
                if self.match_char('=') {
                    return self.make_token(TokenKind::LessEqual);
                } else {
                    return self.make_token(TokenKind::Less);
                }
            }
            '>' => {
                if self.match_char('=') {
                    return self.make_token(TokenKind::GreaterEqual);
                } else {
                    return self.make_token(TokenKind::Greater);
                }
            }
            '"' => {
                return self.string();
            }
            _ => {}
        }

        return self.error_token("Unexpected character.");
    }

    // Advance to the next character.
    fn advance(&mut self) -> char {
        let c = self.peek();
        self.current += 1;
        c
    }

    fn peek_offset(&self, idx: usize) -> char {
        self.source.chars().nth(self.current + idx).unwrap_or('\0')
    }

    fn peek(&self) -> char {
        self.peek_offset(0)
    }

    fn match_char(&mut self, expected: char) -> bool {
        if self.is_at_end() || self.peek() != expected {
            false
        } else {
            self.current += 1;
            true
        }
    }

    // Returns true when the scanner is exhausted.
    fn is_at_end(&self) -> bool {
        self.current >= self.source.len()
    }

    // Create a new token of given kind.
    fn make_token(&self, tty: TokenKind) -> Token {
        Token::new(
            tty,
            self.start,
            self.current - self.start,
            self.line,
            &self.source,
        )
    }

    // Create a new error token with the specific message.
    fn error_token(&self, message: &'static str) -> Token {
        Token {
            kind: TokenKind::Error,
            start: self.start,
            length: self.current - self.start,
            line: self.line,
            source: message,
        }
    }

    fn string(&mut self) -> Token<'_> {
        while self.peek() != '"' && !self.is_at_end() {
            if self.peek() == '\n' {
                self.line += 1;
            }
            _ = self.advance();
        }

        if self.peek() != '"' {
            self.error_token("Unterminated string")
        } else {
            _ = self.advance();
            self.make_token(TokenKind::String)
        }
    }

    fn number(&mut self) -> Token<'_> {
        while self.peek().is_ascii_digit() {
            _ = self.advance();
        }

        // Handle fraction.
        if self.peek() == '.' {
            self.advance();

            while self.peek().is_ascii_digit() {
                _ = self.advance();
            }
        }

        self.make_token(TokenKind::Number)
    }

    fn identifer(&mut self) -> Token<'_> {
        while self.peek().is_ascii_alphanumeric() || self.peek() == '_' {
            self.advance();
        }
        self.make_token(self.identifer_type())
    }

    fn identifer_type(&self) -> TokenKind {
        match self.source.chars().nth(self.start).unwrap() {
            'a' => return self.check_keyword(1, "nd", TokenKind::And),
            'c' => return self.check_keyword(1, "lass", TokenKind::Class),
            'e' => return self.check_keyword(1, "lse", TokenKind::Else),
            'f' => {
                if self.current - self.start > 1 {
                    match self.source.chars().nth(self.start + 1).unwrap() {
                        'a' => return self.check_keyword(2, "alse", TokenKind::False),
                        'o' => return self.check_keyword(2, "r", TokenKind::For),
                        'u' => return self.check_keyword(2, "n", TokenKind::Fun),
                        _ => {}
                    }
                }
            }
            't' => {
                if self.current - self.start > 1 {
                    match self.source.chars().nth(self.start + 1).unwrap() {
                        'h' => return self.check_keyword(2, "is", TokenKind::This),
                        'r' => return self.check_keyword(2, "ue", TokenKind::True),
                        _ => {}
                    }
                }
            }
            'i' => return self.check_keyword(1, "f", TokenKind::If),
            'n' => return self.check_keyword(1, "il", TokenKind::Nil),
            'o' => return self.check_keyword(1, "r", TokenKind::Or),
            'p' => return self.check_keyword(1, "rint", TokenKind::Print),
            'r' => return self.check_keyword(1, "eturn", TokenKind::Return),
            's' => return self.check_keyword(1, "uper", TokenKind::Super),
            'v' => return self.check_keyword(1, "ar", TokenKind::Var),
            'w' => return self.check_keyword(1, "hile", TokenKind::While),
            _ => {}
        }

        return TokenKind::Identifier;
    }

    fn check_keyword(&self, offset: usize, expected: &str, kind: TokenKind) -> TokenKind {
        let keyword_found =
            &self.source[self.start + offset..self.start + offset + expected.len()] == expected;

        let length_matches = (self.current - self.start) == expected.len() + offset;

        if keyword_found && length_matches {
            kind
        } else {
            TokenKind::Identifier
        }
    }
}

//
// Compiler.
//
struct Compiler {}

impl Compiler {
    fn new() -> Self {
        Self {}
    }

    fn compile(&self, source: &str) {
        let mut scanner = Scanner::new(&source);
        let mut line: i32 = -1;
        loop {
            let token = scanner.scan_token();

            if token.line != (line as usize) {
                print!("{:4} ", token.line);
                line = token.line as i32;
            } else {
                print!("   | ");
            }

            println!("{:2} '{}'", token.kind.clone() as u32, token.lexeme());
            io::stdout().flush().unwrap();

            if token.kind == TokenKind::Eof {
                break;
            }
        }
    }
}

//
// Virtual Machine.
//

// The InterpretResult enum symbolises the state of the compiler result.
enum InterpretResult {
    Ok,
    CompileError,
}

// The max size of the stack.
const STACK_MAX: usize = 256;

// The virtual machine (VM) is responsible for interpreting bytecode chunks and mutating internal state accordingly.
struct VM {
    // Bytecode chunks.
    chunk: Chunk,

    // Instruction pointer.
    ip: usize,

    // Stack.
    stack: Vec<Value>,
}

impl VM {
    // Return a new virtual machine instance.
    fn new() -> Self {
        Self {
            chunk: Chunk::new(),
            ip: 0,
            stack: Vec::with_capacity(STACK_MAX),
        }
    }

    // Push a new value onto the stack.
    fn push(&mut self, value: Value) {
        self.stack.push(value);
    }

    // Pop and return value from the stack.
    fn pop(&mut self) -> Value {
        self.stack.pop().unwrap()
    }

    // Interpret source code. Return Interpret result which symbolizes the success state.
    #[allow(unused_variables)]
    fn interpret(&mut self, source: &str) -> InterpretResult {
        let compiler = Compiler::new();
        compiler.compile(source);
        InterpretResult::Ok
    }

    // Interpret the next byte as an opcode.
    fn read_instruction(&mut self) -> Option<Opcode> {
        FromPrimitive::from_u8(self.read_byte())
    }

    // Read the current byte and increment onto the next.
    fn read_byte(&mut self) -> u8 {
        let instruction: u8 = self.chunk.code[self.ip];
        self.ip += 1;
        instruction
    }

    // Read the byte as the value used to index into the constants array.
    fn read_constant(&mut self) -> f32 {
        let idx = self.read_byte() as usize;
        self.chunk.constants[idx]
    }

    // Main run loop. Interpret all byte code and mutate internal state.
    #[allow(dead_code)]
    fn run(&mut self, debug: bool) -> InterpretResult {
        while self.ip < self.chunk.code.len() {
            if debug {
                print!("          ");
                self.stack.iter().for_each(|&slot| print!("[ {} ]", slot));
                println!();
                self.chunk.disassemble_instruction(self.ip);
            }
            match self.read_instruction() {
                Some(Opcode::Constant) => {
                    let constant = self.read_constant();
                    self.push(constant);
                }
                Some(Opcode::Add) => {
                    let a = self.pop();
                    let b = self.pop();
                    self.push(a + b);
                }
                Some(Opcode::Subtract) => {
                    let a = self.pop();
                    let b = self.pop();
                    self.push(a - b);
                }
                Some(Opcode::Multiply) => {
                    let a = self.pop();
                    let b = self.pop();
                    self.push(a * b);
                }
                Some(Opcode::Divide) => {
                    let a = self.pop();
                    let b = self.pop();
                    self.push(a / b);
                }
                Some(Opcode::Negate) => {
                    let negated_value = -self.pop();
                    self.push(negated_value);
                }
                Some(Opcode::Return) => {
                    println!("{}", self.pop());
                    return InterpretResult::Ok;
                }
                None => {
                    println!("Invalid opcode found.")
                }
            }
        }
        InterpretResult::CompileError
    }
}

//
// Main driver.
//
fn main() -> ExitCode {
    let args = Args::parse();
    let vm = VM::new();

    if let Some(path) = args.path.as_deref() {
        run_file(vm, &path)
    } else {
        run_repl(vm)
    }
}

//
// Run file.
//
fn run_file(mut vm: VM, path: &str) -> ExitCode {
    let file_source = std::fs::read_to_string(path);

    if let Ok(file_source) = file_source {
        let result = vm.interpret(&file_source);

        match result {
            InterpretResult::CompileError => ExitCode::from(65),
            InterpretResult::Ok => ExitCode::SUCCESS,
        }
    } else {
        // File not found.
        error!("File at path not found: {}", path);
        io::stdout().flush().unwrap();
        ExitCode::from(74)
    }
}

//
// REPL.
//
fn run_repl(mut vm: VM) -> ExitCode {
    let _ = vm;
    print!("> ");
    io::stdout().flush().unwrap();
    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        let line = line.unwrap();
        match vm.interpret(&line) {
            InterpretResult::CompileError => return ExitCode::from(65),
            _ => {}
        }
        print!("> ");
        io::stdout().flush().unwrap();
    }
    ExitCode::SUCCESS
}

#[cfg(test)]
mod tests {
    use super::*;

    //
    // Testing scanner.
    //
    #[test]
    fn test_scanner_basic() {
        let source = "(";
        let mut scanner = Scanner::new(source.to_string());

        let token = scanner.scan_token();

        assert!(token.kind == TokenKind::LeftParen);
    }

    #[test]
    fn test_scanner() {
        let source = "({;,.-+/*})";
        let mut scanner = Scanner::new(source.to_string());

        let mut idx = 0;
        let expected = vec![
            TokenKind::LeftParen,
            TokenKind::LeftBrace,
            TokenKind::Semicolon,
            TokenKind::Comma,
            TokenKind::Dot,
            TokenKind::Minus,
            TokenKind::Plus,
            TokenKind::Slash,
            TokenKind::Star,
            TokenKind::RightBrace,
            TokenKind::RightParen,
        ];
        while !scanner.is_at_end() {
            let token = scanner.scan_token();

            assert!(token.kind == expected[idx]);

            idx += 1;
        }
    }
}
