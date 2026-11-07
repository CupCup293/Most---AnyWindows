.code16
.text
    mov $0xe9, %dx
    .irp byte,82,85,70,85,83,45,66,79,79,84,45,79,75
    mov $\byte, %al
    out %al, %dx
    .endr
    hlt
