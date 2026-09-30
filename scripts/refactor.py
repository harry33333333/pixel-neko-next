import os
import re

def process_file(filename):
    with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
        text = f.read()

    # Replace v[Y * 320 + X] = C; with PUT_PIXEL(v, X, Y, C);
    # Match things like v[y*320+x] = 7;
    # Match things like v[(y+3)*320+(x+1)] = c;
    
    # We will use a regex to find v[...] = ...;
    # First, let's replace 320 with screen_w in loops just in case? No, the user wants 1024x768.
    # Actually, hardcoded 320 and 200 are everywhere.
    
    # regex for v[Y*320+X] = C;
    # (.*?) is lazy
    new_text = re.sub(r'v\[(.*?)\*320\+(.*?)\]\s*=\s*(.*?);', r'PUT_PIXEL(v, \2, \1, \3);', text)
    
    # For loops with 320 or 200, it's harder.
    # What about v[(y)*320+bx]=0; -> v[(y)*320+(bx)]=0;
    
    if text != new_text:
        with open(filename, 'w', encoding='utf-8') as f:
            f.write(new_text)
        print(f"Refactored {filename}")

for file in os.listdir('.'):
    if file.endswith('.c'):
        process_file(file)
