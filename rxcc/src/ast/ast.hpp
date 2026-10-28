#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rx {

// 源码位置：由 ANTLR token 的 getLine() / getCharPositionInLine() 转换而来。
// 行从 1 开始，列从 0 开始
// 例：源码 `1 + 2` 中 `2` 的起点为 {line=1, column=4}，即第 1 行、零基列号 4。
struct Position {
    int line;   // 行号，从 1 起，例：1
    int column; // 列号，从 0 起，例：5
};

// 一个节点源码区间 [start, end)，用于报错定位。
// 例：`1 + 2` 的整体 → start={1,0}, end={1,5}（终点位于最后一个字符之后）。
struct Span {
    Position start; // 区间起点（含）
    Position end;   // 区间终点（不含）
};

struct AstVisitor;

// 所有 AST 节点的共同基类
// 例：每个具体节点（IntegerLiteral、Function、……）都带 span 供报错定位
struct Node {
    Span span; // 该节点对应的源码区间，例：IntegerLiteral `1` 的 {1,0}..{1,1}

    virtual ~Node() = default;

    virtual void accept(AstVisitor &) = 0;
};

struct Type;

// 表达式分类层
// 例：`1 + 2` → BinaryExpression : Expression；`x` → PathExpression；
//     也包括 return、break 等发散表达式
struct Expression : Node {
    Type *type = nullptr; // 预留槽位，当前未使用；语义检查通过 ExpressionResult 返回类型。
};

// 语句分类层
// 例：`let a = 1;` → LetStatement；`f();` → ExpressionStatement；
//     块中除尾表达式外都是语句
struct Statement : Node {};

// 整数后缀
enum class IntSuffix {
    None,  // 无后缀
    I32,   // i32
    U32,   // u32
    Isize, // isize
    Usize, // usize
};

// 二元运算符
enum class BinaryOperator {
    Add,                // +
    Subtract,           // -
    Multiply,           // *
    Divide,             // /
    Remainder,          // %
    BitwiseAnd,         // &
    BitwiseXor,         // ^
    BitwiseOr,          // |
    ShiftLeft,          // <<
    ShiftRight,         // >>
    Equal,              // ==
    NotEqual,           // !=
    LessThan,           // <
    LessThanOrEqual,    // <=
    GreaterThan,        // >
    GreaterThanOrEqual, // >=
    LogicalAnd,         // &&
    LogicalOr,          // ||
};

enum class UnaryOperator {
    Negative,      // - （前缀取负）
    Not,           // !
    Dereference,   // *
    Borrow,        // &
    MutableBorrow, // &mut
};

// 预留的名称解析槽位取值；当前语义检查直接查询符号，不写回此槽位。
enum class Resolution {
    Unresolved,       // 尚未解析
    Local,            // 局部变量
    TopLevelFunction, // 顶层函数项
    ConstantItem,     // 常量项
    AssociatedItem,   // 关联项（impl 内）
    SelfValue,        // self
};

enum class AssignmentOperator {
    Assign,                  // =
    AddAssignment,           // +=
    SubtractAssignment,      // -=
    MultiplyAssignment,      // *=
    DivideAssignment,        // /=
    RemainderAssignment,     // %=
    BitwiseAndAssignment,    // &=
    BitwiseXorAssignment,    // ^=
    BitwiseOrAssignment,     // |=
    ShiftLeftAssignment,     // <<=
    ShiftRightAssignment,    // >>=
};

// 路径段：名字 + 可选类型泛型实参
// 例：`S`（name="S", generics={}）、`Box::<i32>`（name="Box", generics={i32}）、
//     `self`（name="self"）、`Self`（name="Self"）。
struct PathSegment {
    std::string name; // identifier / self / Self
    std::vector<std::shared_ptr<Type>> generics{};
};

// 常量值（纯值，非节点）
struct ConstantValue {
    enum class Kind {
        Int,  // 整数字面量，如 1
        Bool, // 布尔字面量，如 true / false
        Path,     // 常量项路径，如 a::B
        Negative, // 取负 -x
    };
    Kind kind = Kind::Int;
    int64_t int_value = 0;               // Int，例：`42` 时 = 42
    bool bool_value = false;             // Bool，例：`true` 时 = true
    std::vector<PathSegment> path;       // Path：常量项路径，例：`a::B` 时 = {a, B}
    std::shared_ptr<ConstantValue> operand; // Negative 的操作数，例：`-5` 时 = Int(5)
    IntSuffix suffix = IntSuffix::None;  // Int：整数字面量后缀，例：`1u32` → U32
    Span span{};                         // 该常量值的源码区间（报错定位用）
};

std::string to_string(const ConstantValue &v);        // 打印与报错
std::string to_string(const PathSegment &s);       // 打印与报错
std::string to_string(const std::vector<PathSegment> &segs); // 同上，段间 ::

// 语法类型保留源码结构；类型身份由定型后的 SemanticType 判断。
// 例：`i32` → PathType、`&mut S` → ReferenceType、`[i32; 3]` → ArrayType、`()` → UnitType
struct Type {
    Span span;

    virtual ~Type() = default;
    virtual std::string name() const = 0; // 打印与报错，例："i32"
};

// 语法类型 ()；也用于补齐函数省略的返回类型。
// 例：`fn f() {}` 的返回类型、`let u: () = ();` 的类型标注。
struct UnitType : Type {
    std::string name() const override { return "()"; }
};

// 路径类型：基本类型/结构体/Box/Vec/Self。
// 例：`i32`、`S`（结构体）、`Box<i32>`、`Vec<S>`、`Self`。
struct PathType : Type {
    std::vector<PathSegment> segments; // 例：`Box<i32>` 时 = {Box<i32>}
    std::string name() const override;
};

// 引用类型：&T / &mut T。&&T 是两层嵌套
// 例：`&i32`（is_mut=false, inner=i32）、`&mut S`（is_mut=true, inner=S）。
struct ReferenceType : Type {
    bool is_mut = false; // 例：`&mut S` 时 = true，`&S` 时 = false
    std::shared_ptr<Type> inner; // 例：`&mut S` 时 = S
    std::string name() const override;
};

// 数组类型：[T; N]。N 复用 ConstantValue。
// 例：`[i32; 3]`（element=i32, length=Int(3)）、`[S; MAX]`（length=Path(MAX)）。
struct ArrayType : Type {
    std::shared_ptr<Type> element; // 例：`[i32; 3]` 时 = i32
    ConstantValue length;             // 例：`[i32; 3]` 时 = Int(3)
    std::string name() const override;
};

const char *name(BinaryOperator operation);
const char *name(UnaryOperator operation);
const char *name(AssignmentOperator operation);

// derive 名单的枚举（纯值）：outerAttribute 的 #[derive(..)]。
enum class Derive {
    Copy,      // Copy
    Clone,     // Clone
    PartialEq, // PartialEq
    Eq,        // Eq
};
const char *name(Derive d);

// self 参数的四种形态（纯值）：self / mut self / &self / &mut self
struct SelfParameter {
    enum class Kind {
        Value,            // self
        MutableValue,     // mut self
        Reference,        // &self
        MutableReference, // &mut self
    };
    Kind kind = Kind::Value;
};
const char *name(SelfParameter::Kind k); // "self"/"mut self"/"&self"/"&mut self"

// 函数普通参数 ：identifierBinding COLON typeRef。
// 例：`mut x: i32`（name="x", is_mut=true, type=i32）、`y: u32`（is_mut=false）。
struct Parameter {
    std::string name;    // 例："x"
    bool is_mut = false; // 例：`mut x` 时 = true
    std::shared_ptr<Type> type = nullptr; // 例：`x: i32` 时 = i32
};

// 结构体字段声明 ：identifier COLON typeRef。
// 例：`x: i32`（name="x", type=i32）、`items: Vec<S>`（type=Vec<S>）。
struct StructField {
    std::string name; // 结构体内变量名，例："x"
    std::shared_ptr<Type> type = nullptr; // 例："x: i32" 时 = i32
};

struct IntegerLiteral;
struct BinaryExpression;
struct BooleanLiteral;
struct UnitExpression;
struct PathExpression;
struct UnaryExpression;
struct AssignmentExpression;
struct CastExpression;
struct CallExpression;
struct MethodCallExpression;
struct FieldExpression;
struct IndexExpression;
struct BlockExpression;
struct LetStatement;
struct ExpressionStatement;
struct IfExpression;
struct LoopExpression;
struct WhileExpression;
struct BreakExpression;
struct ContinueExpression;
struct ReturnExpression;
struct ArrayExpression;
struct StructExpression;
struct Function;
struct Struct;
struct Const;
struct InherentImpl;

struct AstVisitor {
    virtual ~AstVisitor() = default;
    virtual void visit(IntegerLiteral &) = 0;
    virtual void visit(BinaryExpression &) = 0;
    virtual void visit(BooleanLiteral &) = 0;
    virtual void visit(UnitExpression &) = 0;
    virtual void visit(PathExpression &) = 0;
    virtual void visit(UnaryExpression &e) = 0;
    virtual void visit(AssignmentExpression &e) = 0;
    virtual void visit(CastExpression &e) = 0;
    virtual void visit(CallExpression &) = 0;
    virtual void visit(MethodCallExpression &) = 0;
    virtual void visit(FieldExpression &) = 0;
    virtual void visit(IndexExpression &) = 0;
    virtual void visit(BlockExpression &) = 0;
    virtual void visit(LetStatement &) = 0;
    virtual void visit(ExpressionStatement &) = 0;
    virtual void visit(IfExpression &) = 0;
    virtual void visit(LoopExpression &) = 0;
    virtual void visit(WhileExpression &) = 0;
    virtual void visit(BreakExpression &) = 0;
    virtual void visit(ContinueExpression &) = 0;
    virtual void visit(ReturnExpression &) = 0;
    virtual void visit(ArrayExpression &) = 0;
    virtual void visit(StructExpression &) = 0;
    virtual void visit(Function &) = 0;
    virtual void visit(Struct &) = 0;
    virtual void visit(Const &) = 0;
    virtual void visit(InherentImpl &) = 0;
};

// 整数字面量
// value = 数值；raw = 原始拼写
// 例：`42u32` → value=42, raw="42u32", suffix=U32；`7` → value=7, raw="7", suffix=None
struct IntegerLiteral : Expression {
    int64_t value;      // 例：`42u32` 时 = 42
    std::string raw;    // 原始拼写，例："42u32"
    IntSuffix suffix;   // 例：`42u32` 时 = U32

    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 二元表达式：运算符 + 左右操作数。
// 例：`1 + 2` → operation=Add, left=IntegerLiteral(1), right=IntegerLiteral(2)；`a && b` → operation=LogicalAnd
struct BinaryExpression : Expression {
    BinaryOperator operation;                        // 例：`1 + 2` 时 = Add
    std::shared_ptr<Expression> left; // 左操作数（先求值），例：IntegerLiteral(1)
    std::shared_ptr<Expression> right; // 右操作数，例：IntegerLiteral(2)

    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 布尔字面量
// 例：`true` → value=true；`false` → value=false
struct BooleanLiteral : Expression {
    bool value; // 例：true
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 单元值 ()：空括号
// 例：`let u = ();` 的初始化表达式
struct UnitExpression : Expression {
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 一元表达式：前缀运算符递归（- -x → Negative(Negative(x))）。
// 例：`-x` → operation=Negative, operand=Path(x)；`!b` → operation=Not；`&mut a[0]` → operation=MutableBorrow
struct UnaryExpression : Expression {
    UnaryOperator operation;                                  // 例：`-x` 时 = Negative
    std::shared_ptr<Expression> operand;      // 例：Path(x)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 赋值按右结合构树：a = b = c → Assign(a, Assign(b, c))。
// 语义检查另行验证 left 是可写 place；AST 也保留不满足此条件的语法结构。
// 例：`a = 1` → operation=Assign, left=Path(a), right=IntegerLiteral(1)；`x += 2` → operation=AddAssignment
struct AssignmentExpression : Expression {
    AssignmentOperator operation;                     // 例：`a = 1` 时 = Assign
    std::shared_ptr<Expression> left; // 待验证的赋值目标，例：Path(a)
    std::shared_ptr<Expression> right; // 右侧值，例：IntegerLiteral(1)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// as 转换：左折叠，x as T as U → Cast(Cast(x,T),U)。
// 例：`x as u32` → operand=Path(x), target=u32
struct CastExpression : Expression {
    std::shared_ptr<Expression> operand; // 例：Path(x)
    std::shared_ptr<Type> target;        // 例：u32
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 调用的 callee 保留为表达式节点，可表示路径或调用链等语法结构。
// 当前语义检查只接受路径 callee；能构建 AST 不代表调用合法。
// 例：`f(1, 2)` → callee=Path(f), arguments={IntegerLiteral(1), IntegerLiteral(2)}
//     `S::new()` → callee=Path(S::new), arguments={}
struct CallExpression : Expression {
    std::shared_ptr<Expression> callee;            // 例：Path(f)
    std::vector<std::shared_ptr<Expression>> arguments; // 例：{IntegerLiteral(1), IntegerLiteral(2)}
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 方法调用：x.m(...)。method 是路径段：名字 + 可选 turbofish（DOT
// pathExprSegment）。
// 例：`x.get()` → receiver=Path(x), method="get", arguments={}
//     `v.push::<i32>(1)` 的语法结构 → method={name="push", generics={i32}}；语义检查会拒绝方法类型实参。
struct MethodCallExpression : Expression {
    std::shared_ptr<Expression> receiver;          // 例：Path(x)
    PathSegment method;                            // 例："get"
    std::vector<std::shared_ptr<Expression>> arguments; // 例：{IntegerLiteral(1)}
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 字段访问：x.f
// 例：`s.x` → base=Path(s), field="x"；`self.x` → base=Path(self)
struct FieldExpression : Expression {
    std::shared_ptr<Expression> base; // 例：Path(s)
    std::string field;                // 例："x"
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 索引：a[i]
// 例：`arr[0]` → base=Path(arr), index=IntegerLiteral(0)；`a[i] = 1` 中的左部
struct IndexExpression : Expression {
    std::shared_ptr<Expression> base;  // 例：Path(arr)
    std::shared_ptr<Expression> index; // 例：IntegerLiteral(0)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 块表达式：语句序列 + 可选尾表达式。
// 有 tail 时通常由它决定块结果；无 tail 时，语义检查按语句结果区分 ()、! 和 Error。
// 构建器把文法中的末尾无分号块类表达式也归一化为 tail。
// 例：`{ let a = 1; a + 2 }` → statements={LetStatement(a)}, tail=Binary(Add)
//     `{ }` → statements={}, tail=nullptr（值为 ()）
struct BlockExpression : Expression {
    std::vector<std::shared_ptr<Statement>> statements; // 例：{LetStatement(a)}
    std::shared_ptr<Expression> tail; // 可空：无尾表达式的块，例：Binary(a + 2)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// let 语句：initializer 语法强制必有（equalsSign expression 无 ?）。
// 例：`let mut x: i32 = 1;` → name="x", is_mut=true, type_annotation=i32, initializer=IntegerLiteral(1)
//     `let y = 2;` → type_annotation=nullptr
struct LetStatement : Statement {
    std::string name;                      // 例："x"
    bool is_mut = false;                   // identifierBinding 的 MUT?，例：`mut x` 时 = true
    std::shared_ptr<Type> type_annotation; // 可空，例：`x: i32` 时 = i32；`let y = 2` 时 = nullptr
    std::shared_ptr<Expression> initializer;      // 例：IntegerLiteral(1)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 表达式语句：不保存分号 token，但用 has_semicolon 保留有无分号的信息。
// 包住任意表达式，含省略分号的块类表达式
// 例：`f();` → expression=Call(f)；`if x { 1 };` → expression=IfExpression
struct ExpressionStatement : Statement {
    std::shared_ptr<Expression> expression; // 例：Call(f)
    bool has_semicolon = true;             // 语法上有无 `;`：`{ 1 }` → false，`{ 1 };` → true
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// if 表达式：条件 + then 块 + 可空 else（块或 if——else if 链天然嵌套）。
// then 用具体类型 BlockExpression：语法强制是块，字段能具体就具体；
// else_ 必须用 Expression：ELSE 后面是块或 if 两种可能。
// 例：`if x < 10 { 1 } else if x < 20 { 2 } else { 3 }`：
//     condition=Binary(LessThan), then=Block{1}, else_=IfExpression（else if 嵌套）
struct IfExpression : Expression {
    std::shared_ptr<Expression> condition; // 例：Binary(x < 10)
    std::shared_ptr<BlockExpression> then; // 例：Block{ tail=IntegerLiteral(1) }
    std::shared_ptr<Expression> else_;     // 可空，例：else 块 = Block{3}；无 else = nullptr
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// loop 表达式：无限循环，值由 break 带出
// 例：`loop { break 5; }` → body=Block{ Break(IntegerLiteral(5)) }
struct LoopExpression : Expression {
    std::shared_ptr<BlockExpression> body; // 例：Block{ Break }
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// while：运行时每次迭代前检查条件；静态 walk 按条件、循环体的顺序各访问一次。
// 例：`while x < 10 { x += 1; }` → condition=Binary(LessThan), body=Block{ Assignment(+=) }
struct WhileExpression : Expression {
    std::shared_ptr<Expression> condition; // 例：Binary(x < 10)
    std::shared_ptr<BlockExpression> body; // 例：Block{ x += 1 }
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// break 自身为发散表达式；value 可空，裸 break 为目标 loop 提供 ()，带值时提供该值。
// 例：`break;` → value=nullptr；`break 5;` → value=IntegerLiteral(5)
struct BreakExpression : Expression {
    std::shared_ptr<Expression> value; // 可空，例：IntegerLiteral(5)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// continue 表达式：跳过本次迭代，无字段
// 例：`continue;`
struct ContinueExpression : Expression {
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// return 的 value 可空；语义检查核对返回值后得到 !，不回填 AST 的 type 槽位。
// 例：`return;` → value=nullptr；`return x + 1;` → value=Binary(Add)
struct ReturnExpression : Expression {
    std::shared_ptr<Expression> value; // 可空，例：Binary(x + 1)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 数组表达式：两形——列表 [a, b]（含空 []）与重复 [x; N]。
// 靠 repeat_value 是否为空区分；重复形的元素表达式只求值一次。
// 例：`[1, 2]` → elements={IntegerLiteral(1), IntegerLiteral(2)}, repeat_value=nullptr
//     `[0; 4]` → elements={}, repeat_value=IntegerLiteral(0), repeat_count=Int(4)
struct ArrayExpression : Expression {
    std::vector<std::shared_ptr<Expression>> elements; // 列表形，例：{IntegerLiteral(1), IntegerLiteral(2)}
    std::shared_ptr<Expression> repeat_value; // 重复形：唯一的元素表达式，例：IntegerLiteral(0)
    ConstantValue repeat_count;                  // 重复形：常量次数，例：Int(4)
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 结构体字段的“名字 + 值表达式”记录，自身不继承 Node。
// 例：`S { x: 1, y: 2 }` 中 `x: 1`（name="x", value=IntegerLiteral(1)）。
struct StructExpressionField {
    std::string name;                     // 例："x"
    std::shared_ptr<Expression> value;    // 例：`x: 1` 时 = IntegerLiteral(1)
};

// 结构体表达式：path 复用 PathExpression——语义阶段解析结构体名
// 例：`S { x: 1 }`（path=S, fields={x:1}）、`Point::<i32> { x: 1 }`（path 带泛型）、
//     `S {}`（fields 为空）。
struct StructExpression : Expression {
    std::shared_ptr<PathExpression> path; // 例：`S { .. }` 时 = S；解析结构体名
    std::vector<StructExpressionField> fields;  // 可空（S {}），例：{x:1, y:2}
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 路径表达式在语法上允许多段（a::b::c），具体含义和合法性由使用位置决定。
// resolution 是预留槽位，构建后为 Unresolved，当前语义检查不修改它。
// 例：`x`（segments={x}）、`S::new`（segments={S, new}）、`Box::<i32>::new`（段带泛型）。
struct PathExpression : Expression {
    std::vector<PathSegment> segments; // 至少一段，例：{S, new}
    Resolution resolution = Resolution::Unresolved;
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// —— Item 层：顶层与 impl 内的声明 ——
// 例：`fn f() {}` → Function；`struct S { x: i32 }` → Struct；
//     `const C: i32 = 1;` → Const；`impl S { .. }` → InherentImpl
struct Item : Node {};

// 函数：body 语法强制必有（无 `;` 声明形）。返回类型可省略，语义上是 unit
// 例：`fn add(a: i32, b: i32) -> i32 { a + b }`：
//     name="add", self_parameter=nullptr, parameters={a:i32, b:i32}, 返回类型=i32, body=Block
//     `fn get(&self) -> i32 { self.x }`：self_parameter=Reference（&self）
struct Function : Item {
    std::string name;                      // 例："add"
    std::shared_ptr<SelfParameter> self_parameter; // 可空：普通函数无 self，例：`&self` 时 = Reference
    std::vector<Parameter> parameters;             // 例：{a:i32, b:i32}
    std::shared_ptr<Type> return_type; // 省略 ARROW 时构建器直接填 UnitType 实例，例：i32
    std::shared_ptr<BlockExpression> body; // 例：Block{ tail=Binary(a + b) }
    bool has_generic_parameters = false;       // genericParams? 是否存在，例：`fn f<'a>()` → true
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 结构体定义：derive 列表和字段记录；字段类型使用独立的 Type 层次。
// 例：`#[derive(Copy, Clone)] struct S { x: i32, y: bool }`
//     → name="S", fields={x:i32, y:bool}, derives={Copy, Clone}。
struct Struct : Item {
    std::string name;                  // 结构体名，例："S"
    std::vector<StructField> fields;    // 结构体内变量表，例：{x:i32, y:bool}
    std::vector<Derive> derives;        // #[derive(..)]，例：{Copy, Clone}
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// 常量项：value 复用 ConstantValue
// 例：`const MAX: i32 = 10;` → name="MAX", type=i32, value=Int(10)
struct Const : Item {
    std::string name;           // 例："MAX"
    std::shared_ptr<Type> type; // const identifier COLON typeRef，例：i32
    ConstantValue value;           // 例：Int(10) / Bool(true) / Path(a::B) / Negative(Int(5))
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// inherent impl：items 只出现 Function/Const；self_type 对齐 IMPL 后的
// typeRef。
// 例：`impl S { fn get(&self) -> i32 { self.x } const K: i32 = 1; }`
//     → self_type=S, items={Function(get), Const(K)}
struct InherentImpl : Item {
    std::shared_ptr<Type> self_type;          // 例：S
    std::vector<std::shared_ptr<Item>> items; // 例：{Function(get), Const(K)}
    void accept(AstVisitor &v) override { v.visit(*this); }
};

// crate：AST 根容器，按源码顺序保存已构建的顶层声明；use 项不进入此列表。
// 例：整个 .rx 文件 → items={Function(main), Struct(S), InherentImpl(impl S), ...}
struct Crate {
    std::vector<std::shared_ptr<Item>> items; // 例：{Function(main), Struct(S)}
};

// 将 node 的直接 Node 子节点交给 visitor；不自动递归，也不遍历 Type 或 ConstantValue。
// 顺序由 ChildWalker 定义，所有分支均会访问，循环体只访问一次；这是静态遍历。
void walk(Node &node, AstVisitor &visitor);

} // namespace rx
