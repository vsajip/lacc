int main(int argc, char *argv[])
{
    int foo[2];
    int bar[4][2][1];

    foo[0] = 1;
    bar[2][1][0] = 4;

    return foo[0] + bar[2][1][0];
}
