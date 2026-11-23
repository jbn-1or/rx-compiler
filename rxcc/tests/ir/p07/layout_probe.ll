; Independent RV32 oracle: derive offsets and array strides from LLVM GEPs.
; The types mirror the source fixture in check_named_storage_layouts().
target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"
target triple = "riscv32-unknown-unknown-elf"

%Inner = type { i8, i32, i8 }
%Outer = type { i8, [2 x %Inner], %Inner, i8 }
%Node = type { { ptr, i32, i32 }, ptr }
%Matrix = type { [2 x [3 x i8]], ptr, i32 }
%AlignInner = type { i8, %Inner }

declare void @println_i32(i32)

; Print the byte distance between two pointers into the same allocation.
define internal void @print_offset(ptr %base, ptr %address) {
entry:
  %start = ptrtoint ptr %base to i32
  %end = ptrtoint ptr %address to i32
  %offset = sub i32 %end, %start
  call void @println_i32(i32 %offset)
  ret void
}

define i32 @main() {
entry:
  %inner = alloca [2 x %Inner]
  %outer = alloca [2 x %Outer]
  %node = alloca [2 x %Node]
  %matrix = alloca [2 x %Matrix]
  %align = alloca %AlignInner

  ; Mixed-field offsets, array stride including tail padding, and alignment.
  %flag = getelementptr %Inner, ptr %inner, i32 0, i32 0
  %value = getelementptr %Inner, ptr %inner, i32 0, i32 1
  %tail = getelementptr %Inner, ptr %inner, i32 0, i32 2
  %inner.next = getelementptr [2 x %Inner], ptr %inner, i32 0, i32 1
  %inner.aligned = getelementptr %AlignInner, ptr %align, i32 0, i32 1
  call void @print_offset(ptr %inner, ptr %flag)
  call void @print_offset(ptr %inner, ptr %value)
  call void @print_offset(ptr %inner, ptr %tail)
  call void @print_offset(ptr %inner, ptr %inner.next)
  call void @print_offset(ptr %align, ptr %inner.aligned)

  ; Aggregate fields following both an array and a named structure.
  %pair = getelementptr %Outer, ptr %outer, i32 0, i32 1
  %embedded = getelementptr %Outer, ptr %outer, i32 0, i32 2
  %outer.tail = getelementptr %Outer, ptr %outer, i32 0, i32 3
  %outer.next = getelementptr [2 x %Outer], ptr %outer, i32 0, i32 1
  call void @print_offset(ptr %outer, ptr %pair)
  call void @print_offset(ptr %outer, ptr %embedded)
  call void @print_offset(ptr %outer, ptr %outer.tail)
  call void @print_offset(ptr %outer, ptr %outer.next)

  ; Vec head offsets and the following Box pointer.
  %len = getelementptr %Node, ptr %node, i32 0, i32 0, i32 1
  %cap = getelementptr %Node, ptr %node, i32 0, i32 0, i32 2
  %box = getelementptr %Node, ptr %node, i32 0, i32 1
  %node.next = getelementptr [2 x %Node], ptr %node, i32 0, i32 1
  call void @print_offset(ptr %node, ptr %len)
  call void @print_offset(ptr %node, ptr %cap)
  call void @print_offset(ptr %node, ptr %box)
  call void @print_offset(ptr %node, ptr %node.next)

  ; Nested bool array row stride and a thin reference to a fixed array.
  %row = getelementptr %Matrix, ptr %matrix, i32 0, i32 0, i32 1
  %refer = getelementptr %Matrix, ptr %matrix, i32 0, i32 1
  %word = getelementptr %Matrix, ptr %matrix, i32 0, i32 2
  %matrix.next = getelementptr [2 x %Matrix], ptr %matrix, i32 0, i32 1
  call void @print_offset(ptr %matrix, ptr %row)
  call void @print_offset(ptr %matrix, ptr %refer)
  call void @print_offset(ptr %matrix, ptr %word)
  call void @print_offset(ptr %matrix, ptr %matrix.next)
  ret i32 0
}
