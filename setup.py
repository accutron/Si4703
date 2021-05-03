from distutils.core import setup, Extension

def main():
    setup(name="Si4703",
        version="1.0.0",
        description="Python Wrapper for the Si4703 FM Tuner Chip",
        author="Allan Irvine",
        author_email="alirvine@gmail.com",
        ext_modules=[Extension("Si4703", sources=["Si4703Driver.c"], libraries=["pthread", "wiringPi"])])
    
if __name__ == "__main__":
    main()
    
