import re
with open('wm.c', 'r', encoding='utf-8') as f:
    c = f.read()
if 'restart_confirm' not in c and 'power_confirm' in c:
    print('Refactoring successful!')
else:
    print('Refactoring failed!')
