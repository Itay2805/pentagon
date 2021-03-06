namespace System;

public class ArgumentNullException : ArgumentException
{

    public ArgumentNullException()
        : base("Value cannot be null.")
    {
    }

    public ArgumentNullException(string paramName)
        : base("Value cannot be null.", paramName)
    {
    }

    public ArgumentNullException(string message, Exception innerException)
        : base(message, innerException)
    {
    }

    public ArgumentNullException(string paramName, string message)
        : base(message, paramName)
    {
    }
    
}