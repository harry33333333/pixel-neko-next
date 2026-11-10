#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_handle_byte(unsigned char scancode);
int keyboard_get_key(void);
int keyboard_get_ctrl(void);
void keyboard_push_key(int key);
void keyboard_set_ctrl(int is_ctrl);

#endif
