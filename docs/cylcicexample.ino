
// Trace program solution for cyclic executive example
// Author: Mauro Dragone

#define FRAME_DURATION_MS 2     // 2ms

unsigned long frameTime = 0;
unsigned long frameCounter = 0;

//----------------------------------------------------------------------------------------------------------------------------------------------
void setup(void)
{
  Serial.begin(9600);
  while(!Serial);
  Serial.println("Ready");
    
}
//----------------------------------------------------------------------------------------------------------------------------------------------

void frame() {
   
   unsigned int slot = frameCounter % 10;

   switch (slot) {
     case 0: JobTask1();             JobTask3();               break;
     case 1:             JobTask2();                           break;
     case 2: JobTask1();                                       break;
     case 3:                                     JobTask4();   break;
     case 4:             JobTask2();                           break;
     case 5: JobTask1();                                       break;
     case 6:             JobTask2();                           break;
     case 7: JobTask1();                                       break;
     case 8: JobTask1();                                       break;
     case 9:             JobTask2();                                 
  }
    
}


//----------------------------------------------------------------------------------------------------------------------------------------------
void loop(void) // Single time slot function of the Cyclic Executive (repeating)
{

  // uncomment this to find execution time of given tasks 
  /*
  unsigned long bT = micros();
  for (int i=0; i<1000; i++) {
    JobTask1();
  }
  unsigned long timeItTook = micros()-bT;
  Serial.print("Duration SerialOutput Job = ");
  Serial.print(timeItTook);
  exit(0);
  */

  
  frame();

  // TO-DO: wait the next frame  
}

//----------------------------------------------------------------------------------------------------------------------------------------------
// Fake tasks
//----------------------------------------------------------------------------------------------------------------------------------------------
// Task 1, takes 0.9ms
void JobTask1(void) 
{
   monitor.jobStarted(1);
   delayMicroseconds(900); 
   monitor.jobEnded(1);
} 

// Task 2, takes 1.8ms
void JobTask2(void) 
{
   monitor.jobStarted(2);
   delayMicroseconds(1800);
   monitor.jobEnded(2); 
} 

// Task 3, takes 1ms
void JobTask3(void) 
{
   monitor.jobStarted(3);
   delayMicroseconds(1000);
   monitor.jobEnded(3); 
} 


// Task 4, takes 2ms
void JobTask4(void) 
{
   monitor.jobStarted(4);
   delayMicroseconds(2000);
   monitor.jobEnded(4); 
} 