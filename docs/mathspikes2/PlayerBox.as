package
{
   import Box2D.Collision.Shapes.b2CircleDef;
   import Box2D.Dynamics.b2Body;
   import Box2D.Dynamics.b2BodyDef;
   import Box2D.Dynamics.b2World;
   import Levels.Level;
   import flash.display.DisplayObject;
   import flash.display.MovieClip;
   import flash.display.Sprite;
   import flash.geom.Matrix;
   import flash.geom.Point;
   
   // Practice Hack PlayerBox with MATHSPIKES 2 (see Shipik.as). Only HitTestObjectControlPoints changed.
   [Embed(source="/_assets/assets.swf", symbol="symbol23")]
   public class PlayerBox extends MovieClip
   {
      
      public var body:b2Body;
      
      public var m_worldIn:b2World;
      
      private var live:Boolean = true;
      
      private var testPoints:Array;
      
      public var m_spriteIn:Sprite;
      
      public var hitPoints:Sprite;
      
      public function PlayerBox(param1:Number, param2:Number, param3:b2World, param4:Sprite)
      {
         var _loc5_:b2BodyDef = null;
         var _loc6_:b2CircleDef = null;
         this.testPoints = new Array();
         this.hitPoints = new Sprite();
         this.hitPoints.graphics.beginFill(16711935,0.8);
         super();
         _loc5_ = new b2BodyDef();
         _loc6_ = new b2CircleDef();
         this.m_worldIn = param3;
         this.m_spriteIn = param4;
         x = param1;
         y = param2;
         param4.x = -this.x + Game.stageWidth / 2;
         param4.y = -this.y + Game.stageHeight / 2;
         _loc6_.radius = width / Level.m_physScale / 2;
         _loc6_.localPosition.Set(0,0);
         _loc6_.density = 1;
         _loc6_.friction = 0.4;
         _loc6_.restitution = 0.2;
         _loc5_.position.Set(param1 / Level.m_physScale,param2 / Level.m_physScale);
         _loc5_.userData = this;
         this.body = param3.CreateBody(_loc5_);
         this.body.SetBullet(true);
         this.body.CreateShape(_loc6_);
         this.body.SetMassFromShapes();
         param4.addChild(this);
         var _loc7_:int = 16;
         var _loc8_:Number = this.width / 2;
         var _loc9_:Number = 2 * Math.PI / _loc7_;
         var _loc10_:int = 0;
         var hpHalfSize:* = 0.2;
         while(_loc10_ < _loc7_)
         {
            this.testPoints.push(new Point(_loc8_ * Math.cos(_loc9_ * _loc10_),_loc8_ * Math.sin(_loc9_ * _loc10_)));
            this.hitPoints.graphics.drawRect(_loc8_ * Math.cos(_loc9_ * _loc10_) - hpHalfSize,_loc8_ * Math.sin(_loc9_ * _loc10_) - hpHalfSize,2 * hpHalfSize,2 * hpHalfSize);
            _loc10_++;
         }
         this.hitPoints.graphics.endFill();
      }
      
      public function HitTestObjectControlPoints(param1:DisplayObject) : Boolean
      {
         var n:int = 0;
         var _loc3_:Point = null;
         var _loc2_:int = 0;
         if(CONFIG.MATHSPIKES == 2)
         {
            // The same control points as localToGlobal makes (local point truncated to twips, 16.16 matrix,
            // rounded to a twip), but in Level coordinates, where the ball lives: no window or zoom involved.
            var bm:Matrix = this.transform.matrix;
            var a16:int = Math.round(bm.a * 65536);
            var b16:int = Math.round(bm.b * 65536);
            var c16:int = Math.round(bm.c * 65536);
            var d16:int = Math.round(bm.d * 65536);
            var X:int = Math.round(this.x * 20);
            var Y:int = Math.round(this.y * 20);
            var q:Point = null;
            var qx:int = 0;
            var qy:int = 0;
            var gx:int = 0;
            var gy:int = 0;
            var px:Number = 0;
            var py:Number = 0;
            while(_loc2_ < 16)
            {
               q = this.testPoints[_loc2_];
               qx = int(q.x * 20);
               qy = int(q.y * 20);
               gx = X + ((a16 * qx + c16 * qy + 32768) >> 16);
               gy = Y + ((b16 * qx + d16 * qy + 32768) >> 16);
               px = gx / 20;
               py = gy / 20;
               if(param1 is Shipik)
               {
                  if((param1 as Shipik).testLevelPoint(px,py))
                  {
                     return true;
                  }
               }
               else if(param1 is Ships10)
               {
                  n = 0;
                  while(n < (param1 as Ships10).numChildren)
                  {
                     if((param1 as Ships10).getChildAt(n) is Shipik)
                     {
                        if(((param1 as Ships10).getChildAt(n) as Shipik).testLevelPoint(px,py))
                        {
                           return true;
                        }
                     }
                     n++;
                  }
               }
               _loc2_++;
            }
            return false;
         }
         while(_loc2_ < 16)
         {
            _loc3_ = this.localToGlobal(this.testPoints[_loc2_]);
            if(CONFIG.MATHSPIKES == 1)
            {
               if(param1 is Shipik)
               {
                  if((param1 as Shipik).testPoint(_loc3_))
                  {
                     return true;
                  }
               }
               else if(param1 is Ships10)
               {
                  n = 0;
                  while(n < param1.numChildren)
                  {
                     if(param1.getChildAt(n) is Shipik)
                     {
                        if((param1.getChildAt(n) as Shipik).testPoint(_loc3_))
                        {
                           return true;
                        }
                     }
                     n++;
                  }
               }
            }
            else if(param1.hitTestPoint(_loc3_.x,_loc3_.y,true))
            {
               return true;
            }
            _loc2_++;
         }
         return false;
      }
      
      public function IsLive() : Boolean
      {
         return this.live;
      }
      
      public function Kill() : void
      {
         if(this.live)
         {
            this.m_worldIn.DestroyBody(this.body);
            this.m_spriteIn.removeChild(this);
            this.live = false;
         }
      }
   }
}

