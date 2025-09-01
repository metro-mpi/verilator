1. The sub-module declarations must be in single line, for example the tile modules are instantianted in 2 lines in the chip modules like this
    
    ```
    tile #(.TILE_TYPE(TYPE_OF_TILE))
    tile0 ( ......
    )
    ```
    Modify this to be in a single line like
    ```
    tile #(.TILE_TYPE(TYPE_OF_TILE)) tile0 
    (......)
    ```
    The tool needs the the module name and instance name in the same line. This could be improved but for now let's stick with this.

2. Avoid using the wires declared in array fashion like 
    ```
    wire [7:0] east_t_0[0:4]; //Avoid using this

    wire [7:0] east_t_0_0, east_t_0_1, east_t_0_2, east_t_0_3;  //Instead use this
    ```

    