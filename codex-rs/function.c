#include <stdio.h>

int main(void) {
    for (int i = 0; i < 5; i++) {
        printf("%d\n", i);
    }
    int j = 0;
    while(j < 5){
        printf("%d\n", j++);
     }
    for (int row =0; row <3; row++){
        for(int col =0; col < 3; col++){
        printf("%d ; %d", row, col);
    }
}

    return 0;
}
