using System;
using System.Collections.Generics;
using System.Runtime.InteropServices;

namespace Pentagon;

public class Kernel
{
    
    public static int Main()
    {
        List<int> a = new List<int>();
        a.Add(1);
        a.Add(2);
        a.Add(3);

        var sum = 0;
        for (var i = 0; i < a.Count; i++)
        {
            sum += a[i];
        }
        return sum;
    }
    
}